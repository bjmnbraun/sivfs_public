#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/delay.h>

#include "sivfs.h"
#include "sivfs_state.h"
#include "sivfs_shared_state.h"
#include "sivfs_commit_update.h"
#include "sivfs_inode_info.h"
#include "sivfs_file_info.h"
#include "sivfs_config.h"
#include "sivfs_log.h"
#include "sivfs_stack.h"
#include "sivfs_mm.h"

//An implementation of sivfs_commit_update that uses checkpoints
//Compare to sivfs_commit_update_directaccess, etc.
//
//It is at this point that we translate a writeset to the properties of the
//files. This can be done outside of a critical section, but we acquire
//mmap_sem just for own sanity.
//
//If some files have different commit permissions than others, this should be
//determined here and pinned down (if they change, that's OK - there was some
//point in time where the permissions were as such.)
//
//This is also where we update nWriteTxnsWrites
static int sivfs_add_metadata_to_changes(
        struct sivfs_state* state,
        struct sivfs_log_entry* log_writeset
){
        int rc = 0;

        //Skip this step if the log_writeset declares it has already tagged
        //metadata. This is useful for fabricated commits as in
        //sivfs_nontxnal_ops.
        //
        //This flag also requires the user to have cleared the dirty flag of
        //any dirty pages.
        if (log_writeset->ws_has_metadata){
                if (SIVFS_DEBUG_COMMIT){
                        dout("Nontxnal op - skipping metadata step");
                }
                return 0;
        }

        //Convenience variables
        struct sivfs_writeset* ws = &log_writeset->ws;
        sivfs_ts next_ts = log_writeset->ts;
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        //Lock the mm struct once and we will use it multiple times
        struct mm_struct *mm = current->mm;
        down_read(&mm->mmap_sem);


        struct sivfs_writeset_entry* ent = ws->entries;
        size_t size_left = ws->__size_bytes;
        while(size_left){
                struct sivfs_wse_stream_info info;
                rc = sivfs_wse_to_stream_info(
                        &info,
                        ent,
                        size_left
                );
                if (rc){
                        dout("Here");
                        goto error0;
                }

                state->stats.nWriteTxnsWrites++;

                unsigned long target_start = ent->address;

                //Requires mm lock on current, see above
                struct vm_area_struct* vma;
                rc = sivfs_find_vma(
                        &vma,
                        sstate,
                        target_start,
                        sizeof(sivfs_word_t),
                        true
                );
                if (rc){
                        dout("Here");
                        goto error0;
                }

                if (unlikely(target_start & 7)){
                        dout("Non-word-aligned write found in writeset.");
                        rc = -EINVAL;
                        goto error0;
                }

                //OK compute the file offset
                size_t file_offset =
                        (target_start - vma->vm_start) +
                        ((size_t)vma->vm_pgoff << PAGE_SHIFT)
                ;

                //JEEZ this is shady
                struct sivfs_file_info* finfo = sivfs_vma_to_finfo(vma);
                struct inode* backing_inode = sivfs_vma_to_backing_inode(vma);
                struct sivfs_inode_info* backing_iinfo =
                        sivfs_inode_to_iinfo(backing_inode)
                ;
                loff_t file_size = i_size_read(backing_inode);

                //Check file offset bounds
                if (file_offset >= file_size){
                        dout("Write beyond size of file found in writeset.");
                        //This is not an error, but it does cause abort. A
                        //cause of this can be a concurrent truncate with a
                        //transaction.
                        //
                        //We update backing_inode's size before committing
                        //zeros to the truncated region. This means that if we
                        //check out a version with the committed zeros (and
                        //hence could commit on top of it without WW conflict) we
                        //will also see the size update, and hence will abort
                        //for that reason.
                        log_writeset->aborted = true;
                        goto out;
                }

                //We mark the written-to pages as clean. After all, any writes
                //that we want to keep around are in the writeset provided to
                //the commit ioctl. If commit fails or we abort,
                //any modified pages need to be kicked out.
                pgoff_t pgoff = file_offset >> PAGE_SHIFT;

                //Need to clear the dirty bit in both the VMA and on the page.
                //If we don't do the former, then try_to_unmap and friends will
                //mark the page as dirty when cleaning up PTEs, preventing the
                //page from being reclaimed
                //
                //TODO wrapper for the below
                {
                        bool had_page = false;

                        //Ugghhh
                        pgd_t* pgd = pgd_offset(vma->vm_mm, target_start);
                        if (!pgd_present(*pgd))
                                goto done_clear_dirty;
                        pud_t* pud = pud_offset(pgd, target_start);
                        if (!pud_present(*pud))
                                goto done_clear_dirty;
                        pmd_t* pmd = pmd_offset(pud, target_start);
                        if (!pmd_present(*pmd))
                                goto done_clear_dirty;
                        spinlock_t *ptl;
                        pte_t* pte = pte_offset_map_lock(
                                vma->vm_mm,
                                pmd,
                                target_start,
                                &ptl
                        );
                        if (!pte_present(*pte))
                                goto done_clear_dirty;
                        *pte = pte_mkclean(*pte);
                        struct page* page = pfn_to_page(pte_pfn(*pte));
                        //page is mapped in, so it is referenced.
                        if (!page) {
                                WARN(true, "No page despite present pte!");
                                pte_unmap_unlock(pte, ptl);
                                goto done_clear_dirty;
                        }
                        ClearPageDirty(page);
                        pte_unmap_unlock(pte, ptl);

                done_clear_dirty:
                        if (!had_page){
                                //Well, this is weird but could happen if:
                                // 1) the workingsetpage was swapped out (but
                                // then we should be freeing the swap entry...)
                                //  - see sivfs_writepage
                                // 2) the application uses blind writes
                        }
                }

                //Complete the entry with its file ino and offset
                ent->ino = backing_inode->i_ino;
                ent->file_offset = file_offset;

                //Advance
                size_t wse_size = sivfs_wse_size(&info);
                ent = (struct sivfs_writeset_entry*)(
                        (char*)ent + wse_size
                );
                size_left -= wse_size;
        }

        log_writeset->ws_has_metadata = true;

out:

error0:
        up_read(&mm->mmap_sem);
        return rc;
}

HEADER_INLINE bool sivfs_ws_ent_conflicts(
        struct sivfs_writeset_entry* a,
        struct sivfs_writeset_entry* b
){
        if (a->ino == b->ino &&
                a->file_offset == b->file_offset){
                return true;
        }
        return false;
}

static int sivfs_ws_ent_conflicts_log(
        bool* conflicts,
        struct sivfs_writeset_entry* conflict_ent,
        struct sivfs_log_entry* log
){
        int rc = 0;

        //Convenience variables
        struct sivfs_writeset* ws = &log->ws;

        //If the writesets are sorted, we can do this faster. But for now,
        //assume they are not.
        struct sivfs_writeset_entry* ent = ws->entries;
        size_t size_left = ws->__size_bytes;
        while(size_left){
                struct sivfs_wse_stream_info info;
                rc = sivfs_wse_to_stream_info(
                        &info,
                        ent,
                        size_left
                );
                if (rc){
                        dout("Here");
                        goto error0;
                }

                if (sivfs_ws_ent_conflicts(ent, conflict_ent)){
                        if (SIVFS_DEBUG_ABORTS){
                                dout(
                                        "Conflict - aborting! %016lx:%016lx"
                                        " was: %016llx writing: %016llx",
                                        ent->ino,
                                        ent->file_offset,
                                        conflict_ent->value,
                                        ent->value
                                );
                        }
                        *conflicts = true;
                        goto out;
                }

                //Advance
                size_t wse_size = sivfs_wse_size(&info);
                ent = (struct sivfs_writeset_entry*)(
                        (char*)ent + wse_size
                );
                size_left -= wse_size;
        }

        //No conflict found
        *conflicts = false;
out:
error0:
        return rc;
}

static void _sivfs_validate(
        struct sivfs_log_entry* log_writeset,
        struct sivfs_log_entry* other_log
){
	//Merging algorithm
	// 1) Have bloom filter of ops that could impact
	// 2) Check bloom filter against other_log, if miss or other txn aborted return
	// 3) Stall on other_log's commit
	// 4) make a map of its write set, mark some as privitized
	// 5) Play back other_log, overwriting read set entries with a hit in the map
		//also check "abort on privatize" here
	// and overwriting the map with updated write set
	// 6) Throw out the map
        // 7) Recompute our bloom filter of ops could impact

	//Why this works: first, we skip only transactions with no operations that could
	//modify our read set, by conservatively reasoning about the fixed parts of operations.
	//Having found a transction that might, we construct its writeset over our read set.
	//This is the "correct" read set that we should run on to merge with this txn, since
	//we already said that the transactions we skipped can't modify the read set, and we
	//know now the write set of the transaction we are merging with.

	//Finally, we update our read set and writeset and bloom filter of operations. Having done so,
	//it is as if we checked out the timestamp we just merged against to start with. We now
	//repeat the above reasoning for further merges.

        //The worry with the above is that we have to maintain a map and update
        //it as we replay our writes.

//Variants
	//An alternative correct strategy is to just abort at step (3)

	//Also, steps (1-2) can be replaced with the following exact check:
		//For ops A in other_log, go through ops B in writeset
		//If found an A that conflicts with some B, goto 3
		//Otherwise, return

	//The above two variants can be combined, this is what is below.

        //if other aborts, we don't need to validate against it.
        if (other_log->aborted){
                return;
        }

        //Convenience variables
        struct sivfs_writeset* ws = &log_writeset->ws;

        //If the writesets are sorted, we can do this faster. But for now,
        //assume they are not.
        struct sivfs_writeset_entry* ent = ws->entries;
        size_t size_left = ws->__size_bytes;
        while(size_left){
                struct sivfs_wse_stream_info info;
                int rc = sivfs_wse_to_stream_info(
                        &info,
                        ent,
                        size_left
                );
                if (rc){
                        //This should never happen - we already checked this
                        dout("Assertion error");
                        //End in some good way
                        log_writeset->aborted = true;
                        return;
                }

                bool conflicts;
                rc = sivfs_ws_ent_conflicts_log(&conflicts, ent, other_log);
                if (rc){
                        //This should never happen - we already checked this
                        dout("Assertion error");
                        //End in some good way
                        log_writeset->aborted = true;
                        return;
                }

                if (conflicts){
                        log_writeset->aborted = true;
                        return;
                }

                //Advance
                size_t wse_size = sivfs_wse_size(&info);
                ent = (struct sivfs_writeset_entry*)(
                        (char*)ent + wse_size
                );
                size_left -= wse_size;
        }
}

//Validate a transaction over a set of concurrent transactions [ts_start, ts_stop].
//Validation can't fail, in the sense of returning an error code, rather
//any failure during validation (not enough information, etc.) just results in
//abort
static void sivfs_validate(
        struct sivfs_state* state,
        struct sivfs_log_entry* log_writeset,
        sivfs_ts ts_start,
        sivfs_ts ts_stop
){
        int rc = 0;

        if (ts_stop < ts_start){
                dout("Assertion error");
                rc = -EINVAL;
                goto error_abort;
        }

        //Convenience variables
        struct sivfs_writeset* writeset = &log_writeset->ws;

        //For very long transactions with (comparatively) small writesets,
        //we can do the following to "leap ahead" whole checkpoints where there
        //are no conflicting writes. To do so,
        //
        //while(true){
        //    next_checkpoint = the next checkpoint >= ts_start
        //      (or infinity if there is no next checkpoint yet)
        //    for ts in [ts_start, min(next_checkpoint, ts_stop)]
        //        validate against ts
        //        if ts == ts_stop goto out;
        //    //ts is a checkpoint, so we can leap ahead
        //    current_checkpoint = ts
        //    next_ts = min(page.obsoleted_ts for all pages written to)
        //          where page is from the current_checkpoint
        //    ts_start = next_ts
        //}

        //Using this system, we only validate (worst case) against
        // CHECKPOINT_INTERVAL * N_MODIFIED_CHECKPOINTS
        //logs, where N_MODIFIED_CHECKPOINTS is the number of
        //checkpoints concurrent to the transaction where the page was
        //modified. However, each "leap" costs npages(writeset) to compute.
        //
        //So the cost is (CHECKPOINT_INTERVAL + sizeof(writeset))* N_MODIFIED_CHECKPOINTS
        //which scales with the product of the size of writeset and #
        //concurrent modifications to that writeset.

        //Another optimization is to use bloomfilters to avoid quadratic
        //comparisons. Bloomfilters are tricky, though, when using sequential
        //write optimizations.

        struct sivfs_log_entry* clog;
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;
        rc = sivfs_ts_to_log_entry(
                &clog,
                sstate,
                ts_start,
                ts_start,
                ts_stop
        );
        if (rc)
                goto error_abort;

        while(1){
                _sivfs_validate(log_writeset, clog);

                if (log_writeset->aborted){
                        goto conflict_abort;
                }

                if (clog->ts == ts_stop){
                        //Done.
                        break;
                }

                struct sivfs_log_entry* next_clog = clog->next;

                if (!next_clog || next_clog->ts > ts_stop){
                        //With timestamps assigned to every integer, this
                        //never occurs, but may as well handle the more
                        //general case where we can non-sequential timestamps
                        break;
                }
                clog = next_clog;
        }

        //Just an extra check
        if (rc)
                goto error_abort;

out:
        return;

error_abort:
        dout("Error in validation");
conflict_abort:
        log_writeset->aborted = true;
}

//Will copy log_writeset, caller must free
static int sivfs_commit_changes(
        sivfs_ts* out_ts,
        bool* aborted,
        struct sivfs_state* state,
        struct sivfs_log_entry* log_writeset
){
        int rc = 0;

        struct sivfs_shared_state* sstate = &sivfs_shared_state;

        //Many things to do:
        // 1) Choose next ts
        // 2) Allocate space in log
        // 3) Write log writeset into log
        // 4) Translate addresses to INO numbers
        // 5) Validate
        // 6) Write out commit status
        // 7) Release ts as committed

        //Reasoning:
        // A: Simplest thing is to just hold commit lock whole time
        // B: Improve over this by moving obviously parallel parts out
        // C: In order to pull parts out need to seq lock at end to release

        //Hence,
        //0: We're going to do 4 first as a seperate operation
        //I: We're going to do 1-3 with a commit lock held
        //II: Then 5,6 in parallel
        //III: Finally 7 with a sequence lock

        //Properties:
        //First, the commit lock hold time is always proportional to num writes
        //Second, validation time is only quadratic for some workloads
        // and for those workloads (all threads updating same counter)
        // it is still completely in parallel

        //Where possible we would like to take the work out of 1-4 and do it
        //before acquiring commit lock, obviously.

	//Probably a win to do some of the validation work here, especially if
	//we ran for a long time.

        //TODO the below is inefficient. It is possible to do the scheme that
        //Deuteronomy does where we allocate space in the log with a single
        //atomic increment and then release the space. This avoids the need for
        //a mutual exclusion section below and increases parallelism. However,
        //currently we don't do this - but we really should!

//I

        //Reconsider use of _irq ? The issue is that we do allocation below. We
        //need to do a different log allocation protocol that does no
        //allocation.

//The following is a hack to force preemption of spin locks. On newer
//kernel, spin_lock will correctly be preempted eventually (good behavior) but on
//4.4 this doesn't seem to happen resulting in deadlock.
//
//Actually, we need to do something similar on all of the spinlocks we
//currently use...
#if 0
        spin_lock(&sstate->commit_lock);
#else
        bool locked = false;
retry_lock:
        ;
        size_t i;
        for(i = 0; i < 10; i++){
            locked = spin_trylock(&sstate->commit_lock);
            if (locked) break;
        }
        if (!locked){
            cond_resched();
            if (signal_pending(current)){
                //Signal pending. Die.
                rc = -EINTR;
                goto error0;
            }
            goto retry_lock;
        }
#endif

        //Allocate a commit number
        sivfs_ts next_ts = atomic64_read(&sstate->commit_started_ts) + 1;
        //Allocate space in the log
        //Will also set the next and prev pointers of the log entries on
        //success
        //Will also insert the log entry into the log cache
        //If anything fails, will roll back and return error
        //
        struct sivfs_log_entry* log_dest;
        rc = sivfs_create_log_entry(&log_dest, sstate, next_ts, log_writeset);
        if (rc){
                goto error_unlock;
        }

//POINT OF NO RETURN. Now that the space is allocated in the log, we must abort
//or commit. Luckily, nothing we do below could really error out anyway, so
//this is not a big deal.

        //Update commit_started_ts, if we don't do this the ts remains
        //available
        atomic64_set(&sstate->commit_started_ts, next_ts);

        //Write log writeset, set timestamp
        sivfs_writeset_copy(&log_dest->ws, &log_writeset->ws);
        log_dest->ts = next_ts;
        log_dest->aborted = false;

        spin_unlock(&sstate->commit_lock);

//II:
        //Validate in parallel
        //Any transactions that have
        //commit_status_filled_in && aborted we can skip (optionally)
        if (next_ts >= state->ts + 2){
                //At least one concurrent transaction. Otherwise, the range of
                //concurrent timestamps is not a proper range and it's not nice
                //to pass improper ranges into functions as arguments.

                sivfs_validate(state, log_dest, state->ts + 1, next_ts - 1);
        }

        //Finally write out our commit state
        log_dest->commit_status_filled_in = true;

//III:
        //Acquire sequence lock for the final parts of commit
        while(atomic64_read(&sstate->master_ts) != next_ts - 1){
        }

        //Release sequence lock
        atomic64_set(&sstate->master_ts, next_ts);

        *aborted = log_dest->aborted;
        *out_ts = next_ts;

        if (SIVFS_DEBUG_COMMIT) dout("Successfully committed %llu", next_ts);


        //Wake up checkpoint workers, we have a commit to checkpoint
        wake_up_interruptible(&sstate->make_checkpoint_wq);

        //Write throttling. Throttle writes. throttle.
        //While next_ts >> latest checkpoint, sleep.
        //If we receive ANY signal, we abort out of this - and hence signals
        //can be used to defeat write throttling but hey it's just a best
        //effort approach anyway!
        struct sivfs_checkpoints* checkpoints = &sstate->checkpoints;
        bool wasThrottled = false;
        while(1){
                sivfs_ts checkpoint_ts =
                        atomic64_read(&checkpoints->latest_checkpoint_published)
                ;
                if (next_ts > checkpoint_ts + SIVFS_MAX_WRITE_TXNS_SINCE_CHECKPOINT){
                        wasThrottled = true;
                        cond_resched();
                        if (signal_pending(current)){
                                //No need to error on this case.
                                break;
                        }
                        //udelay(1);
                        //msleep_interruptible(1);
                } else {
                        break;
                }
        }
        if (wasThrottled){
                state->stats.nWriteTxnsThrottledOnSS++;
        }

out:
        return rc;

error_unlock:
        spin_unlock(&sstate->commit_lock);

error0:
        return rc;
}

static int _sivfs_apply_log_entry_workingset(
        struct sivfs_log_entry* log,
        struct sivfs_state* state
){
        int rc = 0;
        struct sivfs_writeset* ws = &log->ws;

        if (SIVFS_DEBUG_UPDATE) {
                dout("Applying version %llu to workingset", log->ts);
        }

        //Skip aborted logs
        if (log->aborted){
                /*
                //Too noisy.
                if (SIVFS_DEBUG_ABORTS) {
                        dout("Skipping aborted txn %llu", log->ts);
                }
                */
                goto out;
        }

        //TODO bloom filters can be used here.

        struct sivfs_writeset_entry* ent = ws->entries;
        size_t size_left = ws->__size_bytes;
        while(size_left){
                struct sivfs_wse_stream_info info;
                rc = sivfs_wse_to_stream_info(
                        &info,
                        ent,
                        size_left
                );
                if (rc){
                        dout("Here");
                        goto error0;
                }

                //struct page is fine, working set pages are always in main
                //memory unless they are swapped
                struct page* pg;
                //If the page has been swapped out, should swap it back in
                rc = sivfs_find_get_wspg(
                        &pg,
                        state,
                        &state->workingset,
                        ent->ino,
                        ent->file_offset,
                        SIVFS_EVICT_CHECKPOINT
                );
                if (rc)
                        goto error0;

                if (!pg){
                        //No working set page entry (or we evicted a
                        //checkpoint page)
                        goto continue_loop;
                }

                if (sivfs_is_checkpoint_page(pg)){
                        dout("Assertion error");
                        rc = -EINVAL;
                        goto error0;
                }

                void* mem = kmap(pg);
                sivfs_apply_ws_ent_pg(
                        mem,
                        ent
                );
                kunmap(pg);

                //TODO uncomment this
#if 0
                if (PageDirty(pg)){
                        dout("Weird...");
                }
#endif

                sivfs_put_wspg(pg);

continue_loop:
                ;
                //Advance
                size_t wse_size = sivfs_wse_size(&info);
                ent = (struct sivfs_writeset_entry*)(
                        (char*)ent + wse_size
                );
                size_left -= wse_size;
        }

out:
error0:
        return rc;
}

//Given a log entry, apply all changes in that log to the working set
//and do the same for all subsequent logs up to, and including, a log with
//timestamp ts_stop.
//
//That is, the log must have valid "next" pointers up to ts_stop, otherwise
//this will throw an assertion error
//
//"skips" tne log entry with ts ts_skip, if any
static int sivfs_apply_logs_workingset(
        struct sivfs_log_entry* log,
        sivfs_ts ts_stop,
        sivfs_ts ts_skip,
        struct sivfs_state* state
){
        int rc = 0;

        struct sivfs_log_entry* clog = log;

        if (clog->ts > ts_stop){
                dout("Assertion error: %llu %llu", clog->ts, ts_stop);
                rc = -EINVAL;
                goto error0;
        }

        //After loop, clog is the last log with ts <= ts_stop
        while(1){
                if (clog->ts == ts_skip){
                        goto skip;
                }

                rc = _sivfs_apply_log_entry_workingset(
                        clog,
                        state
                );
                if (rc) {
                        dout("Here");
                        goto error0;
                }

skip:
                if (clog->ts == ts_stop){
                        //Done.
                        break;
                }

                struct sivfs_log_entry* next_clog = clog->next;

                if (!next_clog || next_clog->ts > ts_stop){
                        //With timestamps assigned to every integer, this
                        //never occurs, but may as well handle the more
                        //general case where we can non-sequential timestamps
                        break;
                }
                clog = next_clog;
        }
error0:
        return rc;
}

//Will skip timestamp "skip_ts" (can be INVALID_TS) when applying changes. This
//is a very minor optimization that allows us to skip applying our own writes.
//
//Note that the optimization doesn't allow for blind writes, TODO support for
//blind writes - we can just always apply log entries containing blind writes...
static int sivfs_update(
        struct sivfs_state* state,
        sivfs_ts next_ts,
        sivfs_ts skip_ts
){
        int rc = 0;

        if (state->direct_access){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        if (next_ts == SIVFS_INVALID_TS){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        //TODO need a check here if sivfs->ts is valid but very old, consider
        //just wiping the whole workingset and then taking the update shortcut
        //below.

        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        if (state->ts == SIVFS_INVALID_TS) {
                //Shortcut. If we don't have any pages mapped in,
                //nothing to do but update ts
                //
                //Can't take this even if SIVFS_TRACK_CLEAR_WORKINGSET_EVERY_TXN
                //since we want to invalidate checkpoint pages we have mapped in
                state->ts = next_ts;
        } else {
                //Apply all changes after start_ts up to next_ts
                sivfs_ts start_ts = state->ts + 1;

                if (start_ts > next_ts){
                        //Nothing to apply
                        if (SIVFS_DEBUG_UPDATE) dout("Skipping update");
                        goto out1;
                }

                if (SIVFS_DEBUG_UPDATE) {
                        dout(
                                "Updating from %llu to %llu",
                                state->ts,
                                next_ts
                        );
                }

                //TODO Have to go through vmas and if they have out-of-date
                //pages OR INODE ATTRIBUTES (size) and update them

                //Aside from the technique mentioned above (just flush
                //workingset and jump ahead) another approach is to use the
                //same technique proposed in fast validation where we jump
                //ahead whole checkpoints if they don't modify any page we have
                //mapped in. However, it is less likely to work here since
                //workingsets are likely to be much larger than transaction
                //writesets.

                //i.e. might call
                //rc = sivfs_set_attributes_txn_ts(state, file->f_inode);
                struct sivfs_log_entry* log;
                rc = sivfs_ts_to_log_entry(
                        &log,
                        sstate,
                        start_ts,
                        start_ts,
                        next_ts
                );
                if (rc) {
                        dout("Here");
                        goto error0;
                }
                if (log == NULL){
                        dout("Assertion error");
                        rc = -EINVAL;
                        goto error0;
                }
                rc = sivfs_apply_logs_workingset(
                        log,
                        next_ts,
                        skip_ts,
                        state
                );
                if (rc) {
                        dout("Here");
                        goto error0;
                }
out1:
                state->ts = next_ts;
        }

error0:
        return rc;
}

static int sivfs_update_direct_access(
        struct sivfs_state* state,
        sivfs_ts next_ts
){
        int rc = 0;
        struct sivfs_shared_state* sstate = &sivfs_shared_state;

        if (next_ts == SIVFS_INVALID_TS){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        sivfs_ts checkpoint_ts = atomic64_read(&state->checkpoint_ts);
        if (next_ts != checkpoint_ts){
                dout("Couldn't check out direct access. (%llu %llu)", checkpoint_ts, next_ts);
                rc = -EINVAL;
                goto error0;
        }

        //There can be no outstanding commits. TODO need to keep the commit
        //lock held somehow to ensure this stays true?
        spin_lock(&sstate->commit_lock);
        sivfs_ts commit_started_ts = atomic64_read(&sstate->commit_started_ts);
        spin_unlock(&sstate->commit_lock);
        if (commit_started_ts != next_ts){
                dout("Couldn't check out direct access.");
                rc = -EINVAL;
                goto error0;
        }

        //TODO acquire some sort of direct access lock on all open files. 
        //Opening / closing files within direct access is not allowed.
        //Release the locks on death or end of direct access.

        //Flush workingset pages if we are entering direct access because we
        //want to fault in the checkpoint pages
        if (!state->direct_access){
                rc = sivfs_invalidate_workingset(
                        &state->workingset,
                        SIVFS_NO_INVALIDATE_CPS
                );
                if (rc) {
                        dout("Here");
                        goto error0;
                }
        }

        //Set ts
        state->ts = next_ts;

        //Set direct access flag, allowing us to directly modify checkpoint
        //pages
        state->direct_access = true;

error0:
        return rc;
}

static int my_sivfs_commit_update(
        bool* aborted_out,
        struct sivfs_state* state,
        struct sivfs_log_entry* log_writeset,
        int options
){
        int rc = 0;

        bool aborted = false;

        sivfs_ts commit_ts = SIVFS_INVALID_TS;

        if (log_writeset && log_writeset->aborted){
                //Assertion error
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }
        //This is used by kernel routines that want to doom the txn
        if (state->should_abort){
                aborted = true;
                state->should_abort = false;
        }
        //This is used if the user explicitly calls sivfs_abort_txn 
        if (options & SIVFS_ABORT_FLAG){
                aborted = true;
        }

        //Do we have writes to commit?
        if (log_writeset && !sivfs_writeset_empty(&log_writeset->ws)){
                //Translate writeset from addresses into inode numbers
                //Note that we have to do this even if we are already aborted
                //to clear dirty pages properly
                rc = sivfs_add_metadata_to_changes(state, log_writeset);
                if (rc) {
                        dout("Here");
                        //Can just error out quickly here, we haven't touched the log
                        goto error0;
                }

                if (!aborted){
                        //Actually commit the changes
                        //This will also mark any pages containing changes as clean
                        rc = sivfs_commit_changes(
                                &commit_ts,
                                &aborted,
                                state,
                                log_writeset
                        );
                        if (rc) {
                                dout("Here");
                                //This error handling should be sufficient.
                                //We return to the thread, it keeps its
                                //workingset and can continue to fault pages /
                                //try its writeset again later. Nothing has
                                //been modified in userspace memory, and
                                //nothing has been committed to the kernel
                                //state.
                                goto error0;
                        }
                }

                //Need to invalidate working set on abort
                //For testing, can invalidate working set every transaction.
                //
                //Only need to do this if we are using the page cache
                if (aborted && !SIVFS_TRACK_CLEAR_WORKINGSET_EVERY_TXN){
                        rc = sivfs_invalidate_workingset_log(
                                &state->workingset,
                                log_writeset
                        );
                        if (rc) {
                                dout("Here");
                                goto error0;
                        }
                }
        }

        //Handle allocation hooks
        rc = sivfs_allocation_on_commit(state, aborted, commit_ts);
        if (rc){
                dout("Here");
                goto error0;
        }

        if (aborted && SIVFS_DEBUG_ABORTS){
                dout("Aborting!");
        }

        //Trim the page cache
        if (SIVFS_FIGURE_WORKINGSET_HARDLIMIT_PLUS_ONE == 1){
                //Aggressively kick out any non-checkpoint pages mapped in
                rc = sivfs_invalidate_workingset(
                        &state->workingset,
                        SIVFS_NO_INVALIDATE_CPS
                );
                if (rc) {
                        dout("Here");
                        goto error0;
                }
        } else if (SIVFS_FIGURE_WORKINGSET_HARDLIMIT_PLUS_ONE == -1){
                //No need to trim. If we hit memory limits vmscan will
                //automatically kick out workingset pages
        } else {
                //TODO need to keep track of the workingset cache pages and
                //selectively evict a few
                dout("Not yet implemented");
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        struct sivfs_checkpoints* checkpoints = &sstate->checkpoints;

        //Pick a new snapshot to checkout, or reuse the one we have if we can
        struct sivfs_latest_checkpoint* latest_cp =
                &checkpoints->latest_checkpoint
        ;

        //Shortcut - if the checkpoint we had checked out is still valid, keep
        //it.
        sivfs_ts checkpoint_ts = atomic64_read(&latest_cp->ts);
        bool reuse_checkpoint = false;
        bool reuse_ts = false;
        if (likely(atomic64_read(&state->checkpoint_ts) == checkpoint_ts)){
                reuse_checkpoint = true;

                goto _reuse_checkpoint;
        }

        //Double-read-lock of latest_cp
        while(1){
                //Read the actual checkpoint pointer
                state->checkpoint = latest_cp->checkpoint;
                //Write out, then do a store-to-load dependency
                atomic64_set(&state->checkpoint_ts, checkpoint_ts);
                barrier();
                //This read should always be redundant, but do it to force
                //store-to-load dependency
                checkpoint_ts = atomic64_read(&state->checkpoint_ts);
                barrier();
                //Validate
                sivfs_ts checkpoint_check = atomic64_read(&latest_cp->ts);
                if (likely(
                        checkpoint_check == checkpoint_ts &&
                        checkpoint_ts != SIVFS_INVALID_TS
                )){
                        //Clean read. Done!
                        break;
                } else {
                        //Dirty read. This should be rare.
                        //Try again from the start:
                        checkpoint_ts = atomic64_read(&latest_cp->ts);
                }
        }
        if (!state->checkpoint){
                //Couldn't check out a checkpoint!
                dout("Couldn't check out a checkpoint.");
                rc = -EINVAL;
                goto error0;
        }

_reuse_checkpoint:
        ;
        //Read next_ts after we safely check out a checkpoint
        sivfs_ts next_ts;

        //External consistency trick - reuse of state->ts
        if (SIVFS_FIGURE_EXTERNAL_CONSISTENCY_TRICKS ==
                SIVFS_FIGURE_YES_EXTERNAL_CONSISTENCY_TRICKS &&
                !aborted &&
                commit_ts == SIVFS_INVALID_TS
        ){
                next_ts = checkpoint_ts;
                if (state->ts != SIVFS_INVALID_TS && state->ts >= next_ts){
                        next_ts = state->ts;
                }
        } else {
                //There is some worry if we sleep here that we will have a large delta
                //between the checkpoint and the transaction version, but this should be
                //extremely rare
                next_ts = atomic64_read(&sstate->master_ts);
        }


        //Rest of procedure does not depend on log_writeset

        //If next_ts == checkpoint_ts and we are reusing last transaction's
        //checkpoint, then next_ts is also equal to that
        //checkpoint's ts, so we can keep checkpoints checked out mapped in.
        //
        //In all other cases, we need to unmap all checkpoints checked out
        //before calling sivfs_update. TODO

        //Wipe the workingset when going from direct access <-> normal mode
        if (options & SIVFS_DIRECT_ACCESS){
                rc = sivfs_update_direct_access(state, next_ts);
                if (rc) {
                        dout("Here");
                        goto error0;
                }
        } else {
                //When leaving direct access, we have to mark all checked out
                //checkpoint pages as read-only. Easier to just kick all
                //pages out.
                if (state->direct_access){
                        rc = sivfs_invalidate_workingset(
                                &state->workingset,
                                0
                        );
                        if (rc) {
                                dout("Here");
                                goto error0;
                        }
                }
                state->direct_access = false;
                rc = sivfs_update(state, next_ts, commit_ts);
                if (rc) {
                        dout("Here");
                        goto error0;
                }
        }

        if (atomic64_read(&state->prior_checkpoint_ts) != checkpoint_ts){
                //No need to hold references to prior_checkpoint_ts.
                //This is akin to "monkey bar" locking, prior_checkpoint_ts is
                //the "old reference" we are letting go of to move to
                //checkpoint_ts, but we have to wait to do this until
                //sivfs_update finished.
                atomic64_set(&state->prior_checkpoint_ts, checkpoint_ts);

                //We have released our hold on some old checkpoint
                //=> wake up gc
                wake_up_interruptible(&sstate->gc_wq);
        }

_reuse_ts:
out:
        *aborted_out = aborted;

error0:
        return rc;
}

//Determines whether file offsets a and b are within the same PAGE_SIZE block
HEADER_INLINE bool sivfs_same_page(size_t a, size_t b){
        return (a / PAGE_SIZE) == (b / PAGE_SIZE);
}

//Overwrites a region of memory of size PAGE_SIZE
//with the currently checked out version
//
//Does not depend on the contents of the region of memory,
//compare this to the method sivfs_apply_logs_workingset which can update
//outdated workingset pages by sequentially applying changes
//
//Caller should do concurrency check on state
//
//Requires rcu read lock held on call
static int my_sivfs_update_page(
        //Region to write into: void* buf with length size
        void* mem,
        struct sivfs_state* state,
        struct vm_area_struct* vma,
        //offset into file, in bytes
        //must be page aligned
        size_t file_offset
) {
        struct sivfs_shared_state* sstate = &sivfs_shared_state;

        int rc = 0;
        int rollback_rc = 0;
        int rollforward_rc = 0;

        //Convenience variable
        sivfs_ts target_ts = state->ts;
        if (target_ts == SIVFS_INVALID_TS){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        if (file_offset % PAGE_SIZE){
                //Then offset is not page aligned.
                //TODO what we should do in this case is call a helper
                //function
                //two times, one time each to fill the part of the range
                //overlapping the current and next page
                dout("Unaligned offset %zd", file_offset);
                rc = -EINVAL;
                goto error0;
        }

        struct inode* file_inode = sivfs_vma_to_backing_inode(vma);
        unsigned long file_ino = file_inode->i_ino;
        struct sivfs_inode_info* file_iinfo =
                sivfs_inode_to_iinfo(file_inode)
        ;

        struct inode* checkpoint = state->checkpoint;
        if (!checkpoint){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        rc = sivfs_read_checkpoint(
                mem,
                PAGE_SIZE,
                checkpoint,
                file_ino,
                file_offset
        );
        if (rc){
                goto error0;
        }

        struct sivfs_inode_info *cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;

        if (!cpinfo){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        //Apply all changes after checkpoint_ts up to target_ts
        sivfs_ts start_ts = cpinfo->cp_ts + 1;
        if (start_ts > target_ts){
                goto out;
        }

        //If we're here, generate a thread-local hashmap of changes happened
        //since updating our workingset. That way, if a subsequent fault
        //happens this transaction we can serve it efficiently.
        //
        //Can even keep that hashmap up to date on future transactions.

        struct sivfs_log_entry* log_ent;
        rc = sivfs_ts_to_log_entry(
                &log_ent,
                sstate,
                start_ts,
                start_ts,
                target_ts
        );
        if (rc)
                goto error0;
        if (log_ent == NULL){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        size_t applied_writes = 0;
        while(1){
                if (log_ent->aborted){
                        /*
                        //Too noisy
                        if (SIVFS_DEBUG_ABORTS) {
                                dout("Skipping aborted txn when updating page");
                        }
                        */
                        goto continue_loop;
                }

                //Convenience variable
                struct sivfs_writeset* ws = &log_ent->ws;

                //Apply all writeset entries in log_ent if they apply
                struct sivfs_writeset_entry* ent = ws->entries;
                size_t size_left = ws->__size_bytes;
                while(size_left){
                        struct sivfs_wse_stream_info info;
                        rc = sivfs_wse_to_stream_info(
                                &info,
                                ent,
                                size_left
                        );
                        if (rc){
                                dout("Here");
                                goto error0;
                        }

                        //If it is a write to the page, apply it
                        if (
                                ent->ino == file_ino &&
                                sivfs_same_page(ent->file_offset, file_offset)
                        ){
                                //accounting
                                applied_writes++;
                                sivfs_apply_ws_ent_pg(
                                        mem,
                                        ent
                                );
                        }

                        //Advance
                        size_t wse_size = sivfs_wse_size(&info);
                        ent = (struct sivfs_writeset_entry*)(
                                (char*)ent + wse_size
                        );
                        size_left -= wse_size;
                }

                struct sivfs_log_entry* next_clog;

continue_loop:
                next_clog = log_ent->next;
                if (!next_clog || next_clog->ts > target_ts){
                        //With timestamps assigned to every integer, this
                        //never occurs, but may as well handle the more
                        //general case where we can non-sequential timestamps
                        break;
                }
                log_ent = next_clog;
        }

        if (SIVFS_DEBUG_PGFAULTS && applied_writes){
                dout("Apply %zd wsents to snapshot page", applied_writes);
        }

out:

error0:
        return rc;
}

//Plug us in
const struct sivfs_commit_update_operations sivfs_commit_update_ops = {
        .commit_update = my_sivfs_commit_update,
        .update_page = my_sivfs_update_page,
};
