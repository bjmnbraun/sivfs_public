#include <linux/cred.h>
#include <linux/slab.h>

#include "sivfs.h"
#include "sivfs_shared_state.h"
#include "sivfs_radix_tree.h"
#include "sivfs_make_checkpoint.h"
#include "sivfs_gc_checkpoints.h"
#include "sivfs_background_threads.h"
#include "sivfs_state.h"

bool sivfs_shared_state_inited = false;
struct sivfs_shared_state sivfs_shared_state;

int sivfs_init_shared_state(
        struct sivfs_shared_state* sstate,
        struct super_block* sb,
        struct sivfs_mount_opts* mount_opts
){
        int rc = 0;

        //Zero out for safety
        memset(sstate, 0, sizeof(*sstate));

        rc = sivfs_init_log(&sstate->master_log);
        if (rc)
                goto error0;

        INIT_RADIX_TREE(&sstate->threads, GFP_KERNEL);
        INIT_RADIX_TREE(&sstate->log_cache, GFP_KERNEL);
        spin_lock_init(&sstate->threads_lock);
        spin_lock_init(&sstate->commit_lock);

        spin_lock_init(&sstate->stats_lock);

        init_waitqueue_head(&sstate->make_checkpoint_wq);
        init_waitqueue_head(&sstate->gc_wq);

        struct cred* dac = prepare_creds();
        if (!dac){
                rc = -ENOMEM;
                goto error1;
        }
        sstate->direct_access_creds = dac;

        sstate->sb = sb;
        sstate->mount_opts = *mount_opts;

        rc = sivfs_init_checkpoints(&sstate->checkpoints);
        if (rc)
                goto error2;

        //Make the first (empty) checkpoint at time 0.
        //TODO might be nice to have some way to load a checkpoint directly
        //from an external source, i.e. fast import of data?

        struct inode* first_checkpoint;
        rc = sivfs_create_checkpoint_inode(&first_checkpoint, sb);
        if (rc){
                dout("Here!");
                goto error3;
        }

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(first_checkpoint)
        ;
        cpinfo->cp_root = NULL;
        cpinfo->cp_ts = 0;

        rc = sivfs_checkpoints_insert(&sstate->checkpoints, first_checkpoint);
        if (rc){
                //Checkpoint not added
                iput(first_checkpoint);
                goto error3;
        }

        //Start up the background work
        rc = sivfs_init_background_work(sstate);
        if (rc)
                goto error3;

out:
        return rc;

error3:
        sivfs_destroy_checkpoints(&sstate->checkpoints);

error2:
        put_cred(sstate->direct_access_creds);

error1:
        sivfs_destroy_log(&sstate->master_log);

error0:
        return rc;
}

void sivfs_delete_log(bool* did_delete, struct sivfs_shared_state* sstate, sivfs_ts stop_ts);

void sivfs_destroy_shared_state(struct sivfs_shared_state* sstate){
        sivfs_destroy_background_work(sstate);

        //threads should be empty
        dout("Destroying shared state");
        if (!sivfs_radix_tree_empty(&sstate->threads)){
                dout("ERROR: Shared state had open sivfs_state's! Leaking memory.");
        }
        void** slot;
        struct radix_tree_iter iter;
        radix_tree_for_each_slot(
                slot,
                &sstate->threads,
                &iter,
                0
        ){
                struct sivfs_state* state = *slot;
                dout(
                        "State had refcount %p %zd",
                        state,
                        atomic64_read(&state->nref)
                );
        }

        //Clean up the log cache and the log entries at the same time
        bool ignored;
        sivfs_delete_log(&ignored, sstate, SIVFS_INVALID_TS);
        if (!sivfs_radix_tree_empty(&sstate->log_cache)){
                dout("ERROR: Shared state had remaining log cache entries! Leaking memory.");
        }

        put_cred(sstate->direct_access_creds);
        sivfs_destroy_checkpoints(&sstate->checkpoints);
        sivfs_destroy_log(&sstate->master_log);
}

//Deletes all entries from the logs before stop_ts. Does not remove the entry
//at stop_ts.
//
//Writes true to did_delete if at least one entry was removed.
//
//Acquires commit lock for some operations.
void sivfs_delete_log(bool* did_delete, struct sivfs_shared_state* sstate, sivfs_ts stop_ts){
        size_t num_nodes = 0;
        unsigned long index;
        int i, nr;
        void** slot;
        struct radix_tree_iter iter;
        const size_t DELETE_BATCH = 128;
        unsigned long indices[DELETE_BATCH];

        index = 0;

        //Acc to gmap_radix_tree_free, the stuff below should work.
        do {
                nr = 0;
                spin_lock(&sstate->commit_lock);
                radix_tree_for_each_slot(
                                slot,
                                &sstate->log_cache,
                                &iter,
                                0
                                )
                {
                        //Free the log entry
                        struct sivfs_log_entry* log_ent = *slot;

                        if (iter.index >= stop_ts){
                                break;
                        }

                        //Actually free the entry (it is still pointed to by
                        //the tree)
                        if (SIVFS_FIGURE_SEQLOG_MODE ==
                                SIVFS_FIGURE_SEQLOG_SEQ
                        ){
                                //First entry in chunk frees last chunk
                                if (log_ent->first_in_chunk){
                                        //Type pun one void* back to get
                                        //address of previous chunk start
                                        void* prev_chunk_start_ptr =
                                                ((void**)log_ent)[-1]
                                        ;
                                        kfree(prev_chunk_start_ptr);
                                }
                        } else {
                                sivfs_put_log_entry(log_ent);
                        }

                        indices[nr] = iter.index;
                        if (++nr == DELETE_BATCH)
                                break;
                }
                if (nr > 0){
                        for(i = 0; i < nr; i++){
                                index = indices[i];
                                radix_tree_delete(&sstate->log_cache, index);
                                *did_delete = true;
                        }
                }
                spin_unlock(&sstate->commit_lock);
        } while (nr > 0);

        if (SIVFS_FIGURE_SEQLOG_MODE ==
                        SIVFS_FIGURE_SEQLOG_SEQ
        ){
                if (stop_ts == SIVFS_INVALID_TS){
                        spin_lock(&sstate->commit_lock);
                        //Also clean up master log unfinished chunk
                        sstate->master_log.bytes_remaining_chunk = 0;
                        kfree(sstate->master_log.chunk_start_ptr);
                        spin_unlock(&sstate->commit_lock);
                }
        }

        //Remember the last stop_ts passed to this for sanity checking
        sstate->master_log_deleted_ts = stop_ts;

}

//Currently ts_stop is ignored, i.e. we don't support paging log entries out to
//disk and loading them back in via such a function. The benefit of doing so is
//not clear, anyway, since very long running transactions should be avoided.
int sivfs_ts_to_log_entry(
        struct sivfs_log_entry** out_log,
        struct sivfs_shared_state* sstate,
        sivfs_ts ts,
        sivfs_ts ts_start,
        sivfs_ts ts_stop
){
        int rc = 0;

        if (ts_start < sstate->master_log_deleted_ts){
                dout("Assertion error: Request for GC'ed log entry");
                rc = -ENOMEM;
                goto error0;
        }

        struct sivfs_log_entry* log = radix_tree_lookup(&sstate->log_cache, ts);
//out:
        *out_log = log;

error0:
        return rc;
}

//Helper transaction for sivfs_create_log_entry
HEADER_INLINE int _sivfs_allocate_log_insert_log_cache(
        struct sivfs_log_entry** out_log,
        struct sivfs_shared_state* sstate,
        sivfs_ts ts,
        struct sivfs_log_entry* log_writeset
){
        int rc = 0;

        //Convenience
        struct sivfs_log* log = &sstate->master_log;

        struct sivfs_log_entry* log_ent = NULL;

        if (SIVFS_FIGURE_SEQLOG_MODE == SIVFS_FIGURE_SEQLOG_SEQ){
                //Do some allocation from a sequential array
                //This will avoid the need for call to writeset_reserve
                size_t sizeof_writeset_entries = log_writeset->ws.__size_bytes;
                size_t bytes_to_allocate =
                        sizeof(struct sivfs_log_entry) +
                        sizeof_writeset_entries
                ;

                void* new_chunk_ptr;
                void* new_chunk_start_ptr;
                size_t new_bytes_remaining_chunk;
                if (log->bytes_remaining_chunk < bytes_to_allocate){
                        //Chunk starts with a pointer to last chunk
                        size_t chunk_bytes_to_allocate =
                                bytes_to_allocate +
                                sizeof(void*)
                        ;
                        size_t chunk_real_allocate = max(
                                (size_t) SIVFS_LOG_CHUNK_SIZE,
                                chunk_bytes_to_allocate
                        );
                        //Zeroing out here not necessary.
                        new_chunk_start_ptr = kmalloc(chunk_real_allocate, GFP_KERNEL);
                        if (!new_chunk_start_ptr){
                                rc = -ENOMEM;
                                goto error0;
                        }
                        new_chunk_ptr = new_chunk_start_ptr;

                        //Write pointer to old chunk at start of new chunk
                        void** prev_chunk_ptr = new_chunk_ptr;
                        prev_chunk_ptr[0] = log->chunk_start_ptr;
                        new_chunk_ptr = (char*)new_chunk_ptr + sizeof(void*);

                        //Space left does not include prev pointer
                        new_bytes_remaining_chunk =
                                chunk_real_allocate - sizeof(void*)
                        ;
                } else {
                        new_chunk_start_ptr = log->chunk_start_ptr;
                        new_chunk_ptr = log->chunk_ptr;
                        new_bytes_remaining_chunk = log->bytes_remaining_chunk;
                }

                log_ent = new_chunk_ptr;
                //Zero out log_ent
                //This is unnecessary.
                memset(log_ent, 0, sizeof(*log_ent));

                new_chunk_ptr = (char*)new_chunk_ptr + sizeof(struct sivfs_log_entry);
                rc = sivfs_init_log_entry(log_ent);
                if (rc){
                        kfree(new_chunk_start_ptr);
                        goto error0;
                }
                log_ent->ws.entries = new_chunk_ptr;
                log_ent->ws.__capacity_bytes = log_writeset->ws.__size_bytes;
                new_chunk_ptr = (char*)new_chunk_ptr + sizeof_writeset_entries;
                //Zero initialization of writeset OK
                //This is unnecessary.
                memset(
                        log_ent->ws.entries,
                        0,
                        sizeof_writeset_entries
                );
                //First in chunk?
                //Account for chunks starting with a prev pointer.
                if ((char*)log_ent ==
                        (char*)new_chunk_start_ptr + sizeof(void*)
                ){
                        log_ent->first_in_chunk = true;
                }

                //Try to insert into radix tree
                rc = radix_tree_insert(&sstate->log_cache, ts, log_ent);
                if (rc){
                        kfree(new_chunk_start_ptr);
                        goto error0;
                }

                //Finally commit changes to log
                log->chunk_start_ptr = new_chunk_start_ptr;
                log->chunk_ptr = new_chunk_ptr;
                log->bytes_remaining_chunk =
                        new_bytes_remaining_chunk - bytes_to_allocate
                ;
        } else {
                //Nonsequential allocation
                rc = sivfs_new_log_entry(&log_ent);
                if (rc){
                        goto error0;
                }

                rc = sivfs_writeset_reserve(
                        &log_ent->ws,
                        log_writeset->ws.__size_bytes
                );
                if (rc){
                        sivfs_put_log_entry(log_ent);
                        goto error0;
                }

                rc = radix_tree_insert(&sstate->log_cache, ts, log_ent);
                if (rc){
                        sivfs_put_log_entry(log_ent);
                        goto error0;
                }
        }


out:
        *out_log = log_ent;

error0:
        return rc;
}

int sivfs_create_log_entry(
        struct sivfs_log_entry** out_log,
        struct sivfs_shared_state* sstate,
        sivfs_ts ts,
        struct sivfs_log_entry* log_writeset
){
        int rc = 0;
        //We need to allocate some space in the log AND add the allocated log
        //pointer into the log_cache. However, both of these could fail and
        //neither are particularly easy to rollback - a speculative entry in
        //log_cache could be problematic for concurrent iterators and
        //deallocating an arbitrary log entry conflicts with sequential-log
        //optimizations. So do this as a transaction.
        struct sivfs_log_entry* log;
        rc = _sivfs_allocate_log_insert_log_cache(
                &log,
                sstate,
                ts,
                log_writeset
        );
        if (rc){
                goto error0;
        }

        //Finally, set prev and next pointers and pdate master_log.latest
        struct sivfs_log_entry* prev_master = sstate->master_log.latest;
        log->prev = prev_master;
        //Important if statement
        if (prev_master){
               prev_master->next = log;
        }
        sstate->master_log.latest = log;

out:
        *out_log = log;

error0:
        return rc;
}
