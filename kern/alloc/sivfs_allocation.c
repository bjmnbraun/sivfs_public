#include "sivfs_allocation.h"

#include "../sivfs_state.h"

//allocator_info_lock must be held
//TODO we can actually relax the lock requirements to allow lockless
//read-only access
//
//Inserts a pginfo if none for a given offset
int sivfs_alloc_get_pginfo(
        struct sivfs_allocator_pginfo** pginfo_out,
        struct sivfs_allocator_info* ainfo,
        size_t offset
){
        int rc = 0;

        size_t pgoff = offset / PAGE_SIZE;

        struct sivfs_allocator_pginfo* pginfo = radix_tree_lookup(
                &ainfo->pginfo_tree,
                pgoff
        );
        if (unlikely(!pginfo)){
                rc = sivfs_new_allocator_pginfo(&pginfo);
                if (rc)
                        goto error0;

                rc = radix_tree_insert(&ainfo->pginfo_tree, pgoff, pginfo);
                if (rc == -EEXIST){
                        dout(
                        "Assertion error: pginfo already existed?"
                        );
                }
                if (rc)
                        goto error_free;
        }

        *pginfo_out = pginfo;

error0:
        return rc;

error_free:
        sivfs_put_allocator_pginfo(pginfo);
        return rc;
}

//NULL if no recovery information, i.e. a fresh file
int sivfs_alloc_info_recover(
        struct sivfs_allocator_info* ainfo,
        void* recovery_info
){
        int rc = 0;
        if (recovery_info){
                //Not yet implemented
                rc = -EINVAL;
                goto error0;
        }

        size_t wild_start = 0;

        //For now, just insert a wilderness from 0 to infty
        struct sivfs_alloc_chunk* wilderness;
        struct sivfs_allocator_pginfo* start_pginfo;
        rc = sivfs_alloc_get_pginfo(&start_pginfo, ainfo, wild_start);
        if (rc) goto error0;
        rc = sivfs_new_alloc_chunk(&wilderness, ainfo);
        if (rc) goto error0;

        start_pginfo->boundary_offset = 0;
        start_pginfo->starts_chunk = wilderness;
        wilderness->size = (size_t)-1;
        list_add(&wilderness->lh_bestfit, &ainfo->bestfit_list);

error0:
        return rc;
}

int sivfs_alloc_block(
        size_t* alloc_out,
        size_t* alloc_size_out,
        struct sivfs_state* state,
        struct sivfs_allocator_info* alloc_info,
        size_t size_requested
){
        int rc = 0;

        //Convenience alias
        struct sivfs_allocator_info* ainfo = alloc_info;

        //Eh, this check is not really necessary (aside from integer overflow)
        //but we do this as a sanity check for now.
        #define MAX_BLOCK_ALLOC_SIZE (64UL*1024UL*1024UL*1024UL)
        if (size_requested > MAX_BLOCK_ALLOC_SIZE){
                dout("Block allocation request too large: %zd", size_requested);
                rc = -EINVAL;
                goto error0;
        }

        //This is also not a real necessity, but enforce it here anyway
        if (size_requested % sizeof(sivfs_word_t)){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        spin_lock(&alloc_info->alloc_info_lock);
        bool locked = true;

        //Check for something in the free list of blocks...
        struct sivfs_alloc_chunk *cur;
        bool found = false;
        size_t nskip = 0;

        //Note: This is not currently a bestfit search - rather it is a first fit
        //search that ignores the wilderness unless it is the only option
        //
        //In practice it seems to work pretty well, though.
        struct sivfs_alloc_chunk *wilderness = NULL;
        list_for_each_entry(
                cur,
                &alloc_info->bestfit_list,
                lh_bestfit
        ){
                if (cur->size < size_requested){
                        //Too small.
                        nskip++;
                        continue;
                }
                if (cur->size == (size_t)-1){
                        wilderness = cur;
                        continue;
                }
                if (!sivfs_alloc_chunk_is_free_visible(cur, state->ts)){
                        if (SIVFS_DEBUG_ALLOC){
                                dout(
                                        "Free chunk not visible - %llu %llu",
                                        cur->last_freed_ts,
                                        state->ts
                                );
                        }
                        continue;
                }
                //We can use it!
                found = true;
                break;
        }

        if (!found && wilderness){
                cur = wilderness;
                found = true;
        }

        if (!found){
                rc = -ENOMEM;
                goto error_unlock;
        }

        if (nskip > 1000){
               dout("Free list traversal of length %zd!", nskip); 
        }

        //We will use chunk "cur"
        bool split = false;
        if (cur->size > size_requested + PAGE_SIZE){
                //Split off at least a page from the end.
                split = true;
        }
        //Split the chunk, if necessary. Add any new chunk afterward.
        //Make sure to update the pginfo too.
        if (split){
                size_t split_boundary = cur->start + size_requested;

                bool has_end = cur->size != (size_t)-1;

                size_t remain_size;
                struct sivfs_allocator_pginfo* bound_pginfo;
                struct sivfs_allocator_pginfo* end_pginfo;
                if (has_end){
                        //The remaining size of cur after we split off size_requested
                        remain_size = cur->size - size_requested;
                } else {
                        //==-1
                        remain_size = cur->size;
                }

                rc = sivfs_alloc_get_pginfo(&bound_pginfo, ainfo, split_boundary);
                if (rc) goto error_unlock;
                if (has_end){
                        size_t cur_end = cur->start + cur->size;
                        rc = sivfs_alloc_get_pginfo(&end_pginfo, ainfo, cur_end);
                        if (rc) goto error_unlock;
                }
                struct sivfs_alloc_chunk* remain_chunk;
                rc = sivfs_new_alloc_chunk(&remain_chunk, ainfo);
                if (rc) goto error_unlock;

                cur->size = size_requested;
                remain_chunk->start = split_boundary;
                remain_chunk->size = remain_size;
                remain_chunk->last_freed_ts = cur->last_freed_ts;
                bound_pginfo->boundary_offset = split_boundary % PAGE_SIZE;
                bound_pginfo->ends_chunk = cur;
                bound_pginfo->starts_chunk = remain_chunk;
                if (has_end){
                        end_pginfo->ends_chunk = remain_chunk;
                }

                //add remain_chunk to the bestfit list
                list_add(
                        &remain_chunk->lh_bestfit,
                        &alloc_info->bestfit_list
                );
        }

        //Move the chunk to thread's allocations list.
        //Also speculatively set it to "allocated" 
        list_move(
                &cur->lh_bestfit,
                &state->allocated_chunks
        );
        cur->last_freed_ts |= SIVFS_CHUNK_ALLOCATED;
        
        if (SIVFS_DEBUG_ALLOC) dout("Allocating %p", cur);

        size_t alloc_ret = cur->start;
        size_t actual_alloc_size = cur->size;

        spin_unlock(&alloc_info->alloc_info_lock);
        locked = false;

        //Need to add the allocation to a list of allocations, to be added to
        //the writeset on commit as metadata-entries. Metadata entries are
        //applied on checkpoint, and on recovery we first make a checkpoint (to
        //ensure metadata is up to date) before scanning to find empty blocks.
        //
        //Also, if the thread dies before the next successful commit, we put
        //the allocation back on the free block list

out:
        *alloc_out = alloc_ret;
        *alloc_size_out = actual_alloc_size;

error_unlock:
        if (locked){
                spin_unlock(&alloc_info->alloc_info_lock);
        }

error0:
        return rc;
}

//sivfs_free_block
//NOTE: Never coalesce back into the "wilderness block!" This is because
//coalescing increases the last_freed_ts and we want the wilderness block to
//always be allocatable for any threads (last_freed_ts stays at 0!)
//
//Also, for a related reason, we want to limit coalescence to be at most some
//fixed multiple of the freed object size - 4x is probably good. This avoids an
//anomaly where the entire heap is being freed and reallocated by a "pestering
//thread" and a slow transaction periodically makes an allocation, which is
//freed shortly thereafter. With
//unbounded coalescence, the pestering thread will force the slow transaction
//to always allocate from the wilderness block - pushing the slow thread's
//allocation farther off into virtual memory space each time. Limiting
//coalescence to 4x the "workingset" of the pestering thread allows the slow
//transaction to always find its allocation without going to the wilderness
//block.
//
//Note also that while doing limited coalescence no block should ever be less
//than a page in size - we need at most one boundary per page!
int sivfs_free_block(
        struct sivfs_state* state,
        struct sivfs_allocator_info* alloc_info,
        size_t block_offset
){
        int rc = 0;

        //Convenience alias
        struct sivfs_allocator_info* ainfo = alloc_info;
        struct sivfs_allocator_pginfo* pginfo;

        spin_lock(&alloc_info->alloc_info_lock);
        bool locked = true;

        rc = sivfs_alloc_get_pginfo(&pginfo, ainfo, block_offset);
        if (rc) goto error_unlock;

        //We can only free a block if all of the below succeed.
        //Note that concurrent activity can make these fail, so they are not
        //necessarily errors if we hit these conditions - just signs of
        //concurrent frees of the same block (which is OK and all but one
        //should abort.)
        if (pginfo->boundary_offset == (size_t)-1){
                state->should_abort = true;
                goto out;
        }
        if (pginfo->boundary_offset != (block_offset % PAGE_SIZE)){
                state->should_abort = true;
                goto out;
        }
        struct sivfs_alloc_chunk* chunk = pginfo->starts_chunk;

        //Annoyingly, we have no easy way to tell if _we_ were the one to 
        //set it free / freeing, so we don't error on double free like we
        //should... Is there a way to get proper double free error? TODO
        if (sivfs_alloc_chunk_is_free(chunk)){
                state->should_abort = true;
                goto out;
        }
        if ((chunk->last_freed_ts & ~SIVFS_CHUNK_ALLOC_FLAGS) > state->ts){
                state->should_abort = true;
                goto out;
        }

        //Set the chunk as freeing
        chunk->last_freed_ts &= ~SIVFS_CHUNK_ALLOCATED;
        chunk->last_freed_ts |= SIVFS_CHUNK_FREEING;

        //note use of lh_to_free instead of lh_bestfit, which may be reserved
        //if the chunk was allocated this txn and hence chunk is on our
        //allocated_chunks list
        list_add(
                &chunk->lh_to_free,
                &state->alloc_chunks_to_free
        );
        if (SIVFS_DEBUG_ALLOC) dout("Freeing %p", chunk);

        spin_unlock(&alloc_info->alloc_info_lock);
        locked = false;

out:

error_unlock:
        if (locked){
                spin_unlock(&alloc_info->alloc_info_lock);
        }

error0:
        return rc;

}

//Assumes lock on alloc_info is held
int sivfs_coalesce_with_next(
        bool* _did_coalesce,
        size_t* coalesce_budget,
        struct sivfs_alloc_chunk* chunk,
        struct sivfs_allocator_info* ainfo
){
        int rc = 0;

        bool did_coalesce = false;

        size_t coalesce_boundary = chunk->start + chunk->size;
        struct sivfs_allocator_pginfo 
                *chunk_pginfo, 
                *coalesce_pginfo,
                *coalesce_end_pginfo
        ;
        rc = sivfs_alloc_get_pginfo(&coalesce_pginfo, ainfo, coalesce_boundary);
        if (rc) goto error0;

        struct sivfs_alloc_chunk* coalesce_chunk =
                coalesce_pginfo->starts_chunk
        ;
        if (!coalesce_chunk){
                //This shouldn't happen unless we're playing games with
                //recovery
                goto out;
        }

        if (coalesce_chunk->last_freed_ts 
                & (SIVFS_CHUNK_FREEING | SIVFS_CHUNK_ALLOCATED)){
                //Coalesce chunk is busy
                goto out;
        }
        if (coalesce_chunk->size == (size_t)-1){
                //Can't coalesce with wilderness chunk
                goto out;
        }
        size_t coalesce_size = coalesce_chunk->size;
        if (coalesce_size > *coalesce_budget){
                //Would go over budget.
                goto out;
        }

        //We can coalesce.
        size_t coalesce_end = coalesce_chunk->start + coalesce_chunk->size;
        rc = sivfs_alloc_get_pginfo(&coalesce_end_pginfo, ainfo, coalesce_end);
        if (rc) goto error0;
        rc = sivfs_alloc_get_pginfo(&chunk_pginfo, ainfo, chunk->start);
        if (rc) goto error0;

        did_coalesce = true;

        chunk->size += coalesce_size;
        *coalesce_budget -= coalesce_size;

        coalesce_pginfo->boundary_offset = (size_t)-1;
        coalesce_pginfo->starts_chunk = NULL;
        coalesce_pginfo->ends_chunk = NULL;

        coalesce_end_pginfo->ends_chunk = chunk;

        list_del(&coalesce_chunk->lh_bestfit);
        sivfs_put_alloc_chunk(coalesce_chunk);

out:
        *_did_coalesce = did_coalesce;

error0:
        return rc;
}

//Assumes lock on alloc_info is held
int sivfs_coalesce_with_prev(
        struct sivfs_alloc_chunk** _merged_with,
        size_t* coalesce_budget,
        struct sivfs_alloc_chunk* chunk,
        struct sivfs_allocator_info* ainfo
){
        int rc = 0;
        struct sivfs_alloc_chunk* merged_with = NULL;

        struct sivfs_allocator_pginfo* chunk_pginfo;
        rc = sivfs_alloc_get_pginfo(&chunk_pginfo, ainfo, chunk->start);
        if (rc) goto error0;

        struct sivfs_alloc_chunk* prev = chunk_pginfo->ends_chunk;
        if (!prev){
                goto out;
        }

        bool did_coalesce;
        rc = sivfs_coalesce_with_next(&did_coalesce, coalesce_budget, prev, ainfo);
        if (rc){
                goto error0;
        }

        if (did_coalesce){
                merged_with = prev;
        }

out:
        *_merged_with = merged_with;

error0:
        return rc;
}

//"Real" free method that frees a block and does coalescence
//Acquires a lock on chunk's alloc_info
//Sets last_freed_ts to commit_ts, if provided. Either way, clears allocated
//flags.
//Only removes SIVFS_CHUNK_ALLOCATED flag, FREEING and such must be 
//removed by caller as well as updating last_freed_ts timestamp
int _sivfs_free_block(struct sivfs_alloc_chunk* chunk, sivfs_ts commit_ts){
        int rc = 0;
        struct sivfs_allocator_info* alloc_info = chunk->ainfo;
        if (!alloc_info){
                //Uh-oh.
                dout("Here");
                return -EINVAL;
        }

        spin_lock(&alloc_info->alloc_info_lock);
        bool locked = true;

        if (commit_ts != SIVFS_INVALID_TS){
                chunk->last_freed_ts = commit_ts;
        }
        chunk->last_freed_ts &= ~SIVFS_CHUNK_ALLOCATED;
        chunk->last_freed_ts &= ~SIVFS_CHUNK_FREEING;

        list_add(
                &chunk->lh_bestfit,
                &alloc_info->bestfit_list
        );

        //Do some coalescence.
        bool did_coalesce;
        size_t coalesce_budget = chunk->size * 4;
        do {
                rc = sivfs_coalesce_with_next(
                        &did_coalesce,
                        &coalesce_budget,
                        chunk,
                        alloc_info
                );
                if (rc){
                        goto error_unlock;
                }
        } while(did_coalesce);

        struct sivfs_alloc_chunk* merged_into;
        do {
                rc = sivfs_coalesce_with_prev(
                        &merged_into,
                        &coalesce_budget,
                        chunk,
                        alloc_info
                );
                if (rc){
                        goto error_unlock;
                }
                chunk = merged_into;
        } while(merged_into);

        spin_unlock(&alloc_info->alloc_info_lock);
        locked = false;

out:

error_unlock:
        if (locked){
                spin_unlock(&alloc_info->alloc_info_lock);
        }

error0:
        return rc;
}

static int _sivfs_allocation_on_commit(
        struct sivfs_state* state,
        sivfs_ts commit_ts
){
        int rc = 0;
        struct sivfs_alloc_chunk *cur, *tmp;

        //Very careful with this method!

        //Just clear the allocated list on commit
        list_for_each_entry_safe(cur, tmp, &state->allocated_chunks, lh_bestfit){
                if (SIVFS_DEBUG_ALLOC) dout("Alloced %p", cur);

                list_del(&cur->lh_bestfit);
        }

        //Now actually free any freed elements
        list_for_each_entry_safe(cur, tmp, &state->alloc_chunks_to_free, lh_to_free){
                if (SIVFS_DEBUG_ALLOC) dout("Freeing %p", cur);

                list_del(&cur->lh_to_free);

                //Commit free. Actually free it. Won't coalesce with any
                //neighboring FREEING elements but that's OK.

                //Two steps: First update cur's last_free_ts, then 
                //call free_block to do the rest. This also removes the
                //FREEING flag.
                rc = _sivfs_free_block(cur, commit_ts);
                if (rc){
                        dout("ERROR: Error in allocation_on_commit!"
                        "Chunk alloc / free status may be incorrect!");
                        rc = -EINVAL;
                        goto error0;
                }
        }

error0:
        return rc;
}

static int _sivfs_allocation_on_abort(
        struct sivfs_state* state
){
        int rc = 0;
        struct sivfs_alloc_chunk *cur, *tmp;
        
        //Very careful with this method!

        //Reset FREEING flags on freed elements
        list_for_each_entry_safe(cur, tmp, &state->alloc_chunks_to_free, lh_to_free){
                if (SIVFS_DEBUG_ALLOC) dout("Aborting free of %p", cur);

                list_del(&cur->lh_to_free);

                //Abort free. Just change flags back to allocated.
                struct sivfs_allocator_info* alloc_info = cur->ainfo;
                if (!alloc_info){
                        dout("ERROR: Error in allocation_on_commit!"
                        "Chunk alloc / free status may be incorrect!");
                        rc = -EINVAL;
                        goto error0;
                }
                //This lock is unnecessary, we are done with cur immediately
                //after the assignment to last_freed_ts. We do need release
                //semantics, though, so just use a lock to be overly safe.
                //
                //Anyway, aborts should be rare
                spin_lock(&alloc_info->alloc_info_lock);
                cur->last_freed_ts = 
                        (cur->last_freed_ts & SIVFS_CHUNK_ALLOC_FLAGS)
                        | SIVFS_CHUNK_ALLOCATED
                ;
                spin_unlock(&alloc_info->alloc_info_lock);
        }

        //Free any allocated chunks
        list_for_each_entry_safe(cur, tmp, &state->allocated_chunks, lh_bestfit){
                if (SIVFS_DEBUG_ALLOC) dout("Aborting alloc of %p", cur);

                list_del(&cur->lh_bestfit);

                //Abort allocation. Free, coalescing only with 
                //neighbor free objects.
                //
                //We pass SIVFS_INVALID_TS to maintain the last freed time
                //(which is exactly what we want since we are aborting)
                rc = _sivfs_free_block(cur, SIVFS_INVALID_TS);
                if (rc){
                        dout("ERROR: Error in allocation_on_commit!"
                        "Chunk alloc / free status may be incorrect!");
                        rc = -EINVAL;
                        goto error0;
                }
        }

error0:
        return rc;
}

int sivfs_allocation_on_commit(
        struct sivfs_state* state,
        bool aborted,
        sivfs_ts commit_ts
){
        //if abort:
        // - first set flags on all freed elements back to "allocated"
        // - next free any allocated blocks
        //The order is important since we might allocate a block and free it in
        //the same txn, in which case we want to unroll in the opposite order.
        //
        //On commit:
        // - clear the allocated list (must happen below the below)
        // - free any freed blocks (deferred free)
        //Order is important since freeing a block trashes its entry in the
        //allocated list.

        //IMPORTANT: chunks have two listnodes since they can be allocated and
        //freed in the same txn.
        //We use lh_to_free for alloc_chunks_to_free and lh_bestfit for
        //allocated_chunks & bestfit lists

        if (likely(!aborted)){
                return _sivfs_allocation_on_commit(state, commit_ts);
        } else { //Aborted!
                return _sivfs_allocation_on_abort(state);
        }
}
