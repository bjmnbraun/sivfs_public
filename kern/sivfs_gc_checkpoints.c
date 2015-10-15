#include <linux/kthread.h>
#include <linux/delay.h>

#include "sivfs_gc_checkpoints.h"
#include "sivfs_shared_state.h"
#include "sivfs_radix_tree.h"
#include "sivfs_stack.h"
#include "sivfs_state.h"

//Looks up the blocked entries list for a particular timestamp
//Creates it if necessary
//gc_blocked_entries_mutex must be held by caller
static int sivfs_gc_get_blocked_entries(
        struct sivfs_stack** out_stack,
        struct sivfs_checkpoints* checkpoints,
        sivfs_ts ts
){
        int rc = 0;

        struct sivfs_stack* stack = radix_tree_lookup(
                &checkpoints->gc_blocked_entries,
                ts
        );
        if (!stack){
                rc = sivfs_new_stack(&stack);
                if (rc)
                        goto error0;
                rc = radix_tree_insert(
                        &checkpoints->gc_blocked_entries,
                        ts,
                        stack
                );
                if (rc){
                        //Need to free the stack since we didn't insert it
                        sivfs_put_stack(stack);
                }
        }

out:
        *out_stack = stack;

error0:
        return rc;
}

/*
//Takes a timestamp and removes (and frees) the blocked pages list at that
//timestamp.
//gc_blocked_entries_mutex must be held by caller
static void sivfs_gc_put_blocked_entries(
        struct sivfs_checkpoints* checkpoints,
        sivfs_ts ts
){
        struct sivfs_stack* stack = radix_tree_lookup(
                &checkpoints->gc_blocked_entries,
                ts
        );
        if (stack){
                radix_tree_delete(
                        &checkpoints->gc_blocked_entries,
                        ts
                );
                sivfs_put_stack(stack);
        }
}
*/

//obsoleted_checkpoint is optional
int sivfs_gc_mark_pages_obsoleted(
        struct sivfs_stack* obsoleted_pages,
        struct inode* obsoleted_checkpoint
){
        int rc = 0;
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        struct sivfs_checkpoints* checkpoints = &sstate->checkpoints;

        //Dumb sanity check?
        if (!obsoleted_pages){
                rc = -EINVAL;
                goto error0;
        }

        //Lock obsoleted_pages
        spin_lock(&checkpoints->obsoleted_entries_lock);
        struct sivfs_stack* obsoleted_entries = &checkpoints->obsoleted_entries;

        //Reserve space for the new entries. Note that we allocate more space
        //than we might actually use, see below.
        size_t new_capacity =
                obsoleted_entries->size + obsoleted_pages->size + 1
        ;
        rc = sivfs_stack_reserve(
                obsoleted_entries,
                new_capacity
        );
        if (rc) {
                goto error_unlock;
        }
        //Now insert them. This could fail.
        size_t i;
        size_t new_size = obsoleted_entries->size;
        for(i = 0; i < obsoleted_pages->size; i++){
                sivfs_gc_entry toAdd;
                rc = sivfs_gc_make_entry_page(&toAdd, obsoleted_pages->values[i]);
                if (rc){
                        //We don't undo the reservation, that's OK
                        goto error_unlock;
                }
                //Put it into obsoleted_entries. Note that size is not yet
                //updated in obsoleted_entries.
                obsoleted_entries->values[new_size] = (void*) toAdd;
                new_size++;
        }
        if (obsoleted_checkpoint){
                sivfs_gc_entry toAdd;
                rc = sivfs_gc_make_entry_inode(&toAdd, obsoleted_checkpoint);
                if (rc){
                        //We don't undo the reservation, that's OK
                        goto error_unlock;
                }
                //Put it into obsoleted_entries. Note that size is not yet
                //updated in obsoleted_entries.
                obsoleted_entries->values[new_size] = (void*) toAdd;
                new_size++;
        }
        //We successfully inserted all elements, update size:
        obsoleted_entries->size = new_size;

out:
        spin_unlock(&checkpoints->obsoleted_entries_lock);
        //Success
        if (SIVFS_DEBUG_GC) dout(
                "Successfully marked %zd pages obsoleted",
                obsoleted_pages->size
        );
        return rc;

error_unlock:
        spin_unlock(&checkpoints->obsoleted_entries_lock);

error0:
        return rc;
}

//Checks if ts is in [start, end), that is, inclusive on start not on end
//end is assumed > start
static bool sivfs_ts_range_contains(
        sivfs_ts start,
        sivfs_ts end,
        sivfs_ts ts
){
        return ts >= start && ts < end;
}

static int sivfs_gc_entry_to_lifetime(
        sivfs_ts* start_ts,
        sivfs_ts* obsoleted_ts,
        sivfs_gc_entry* entry
){
        int rc;
        unsigned long type;

        rc = sivfs_gc_entry_to_type(
                &type,
                entry
        );
        if (rc){
                dout("Here");
                goto error0;
        }

        switch(type){
        case SIVFS_GC_ENTRY_INODE:
                ;
                struct sivfs_inode_info* cpinfo = sivfs_inode_to_iinfo(
                        sivfs_gc_entry_to_inode(entry)
                );
                if (!cpinfo){
                        dout("Here");
                        rc = -EINVAL;
                        goto error0;
                }
                *start_ts = cpinfo->cp_ts;
                *obsoleted_ts = cpinfo->obsoleted_ts;
                break;
        case SIVFS_GC_ENTRY_PAGE:
                ;
                struct sivfs_checkpoint_pginfo* pginfo =
                        sivfs_checkpoint_page_to_pginfo(
                                sivfs_gc_entry_to_page(entry)
                        )
                ;
                if (!pginfo){
                        dout("Here");
                        rc = -EINVAL;
                        goto error0;
                }

                *start_ts = pginfo->checkpoint_ts;
                *obsoleted_ts = pginfo->obsoleted_ts;
                break;
        }

error0:
        return rc;
}

//state is just for stats
//Writes true to did_gc if at least one page freed
static int sivfs_gc_or_reassign_entry(
        bool* did_gc,
        struct sivfs_state* state,
        struct sivfs_checkpoints* checkpoints,
        sivfs_gc_entry* entry,
        struct sivfs_stack* checked_out_versions,
        sivfs_ts latest_snapshot
){
        int rc = 0;

        sivfs_ts start_ts;
        sivfs_ts obsoleted_ts;
        rc = sivfs_gc_entry_to_lifetime(
                &start_ts,
                &obsoleted_ts,
                entry
        );
        if (rc){
                goto error0;
        }

        //Invalid => don't reassign, can free
        sivfs_ts reassign_to = SIVFS_INVALID_TS;
        //obsoleted_ts == INVALID_TS always compares >= to all other ts

        //Is the page still visible in the latest snapshot?
        if (obsoleted_ts > latest_snapshot){
                //reassign to latest_snaphost
                reassign_to = latest_snapshot;
        } else {
                size_t i;
                for(i = 0; i < checked_out_versions->size; i++){
                        sivfs_ts blocker = (sivfs_ts)
                                checked_out_versions->values[i]
                        ;
                        if (sivfs_ts_range_contains(
                                start_ts,
                                obsoleted_ts,
                                blocker
                        )){
                                reassign_to = blocker;
                        }
                }
        }

        if (reassign_to == SIVFS_INVALID_TS){
                if (SIVFS_DEBUG_GC_FREES) dout(
                        "Successfully freeing checkpoint entry @ [%llu %llu) %lu",
                        start_ts,
                        obsoleted_ts,
                        *entry
                );

                sivfs_gc_free_entry(entry, state);

                *did_gc = true;
        } else {
                struct sivfs_stack* reassign_pages;
                rc = sivfs_gc_get_blocked_entries(
                        &reassign_pages,
                        checkpoints,
                        reassign_to
                );
                if (rc)
                        goto error0;

                rc = sivfs_stack_push(
                        reassign_pages,
                        (void*)*entry
                );
                if (rc)
                        goto error0;
        }

error0:
        //We can end up here with an empty set added to gc_blocked_entries
        //that's OK it will be cleaned up on future calls to
        //gc_checkpoints.
        return rc;
}

bool sivfs_gc_blocked_entries_empty(struct sivfs_checkpoints* checkpoints){
        bool empty = true;

        void** slot;
        struct radix_tree_iter iter;
        radix_tree_for_each_slot(
                slot,
                &checkpoints->gc_blocked_entries,
                &iter,
                0
        ){
                struct sivfs_stack* pages = *slot;
                if (pages->size){
                        empty = false;
                        break;
                }
        }

        return empty;
}

//Runs parallel to snapshot generation and cleans up old snapshot pages
int sivfs_gc_checkpoints(bool* did_gc_out){
        int rc = 0;
        bool did_gc = false;
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        struct sivfs_checkpoints* checkpoints = &sstate->checkpoints;

        //Only need it for stats but require it anyway
        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        //Lock gc_blocked_entries
        rc = mutex_lock_interruptible(&checkpoints->gc_blocked_entries_mutex);
        if (rc) {
                //Error'ing on getting a signal is more robust than hanging
                goto error0;
        }

        //Step 0: Fetch as many obsoleted pages as we can and transfer them to
        //gc_blocked_entries[SIVFS_INVALID_TS]. This list might end up nonempty
        //if we fail to recategorize all pages or exit early.

        //it's important that this doesn't fail if we're low on memory,
        //To accomplish this, we don't free the SIVFS_INVALID_TS entry like we
        //do other entries.
        struct sivfs_stack* uncategorized;
        rc = sivfs_gc_get_blocked_entries(
                &uncategorized,
                checkpoints,
                SIVFS_INVALID_TS
        );
        if (rc){
                dout("Here");
                goto error_unlock;
        }

        //Lock obsoleted_pages
        spin_lock(&checkpoints->obsoleted_entries_lock);
        //Does an all-or-nothing push all. If we are close to running out of
        //memory, this will fail.
        rc = sivfs_stack_push_all(
                uncategorized,
                &checkpoints->obsoleted_entries
        );
        //TODO if we fail, we should retry once to just add as much as we
        //can. Remember that we want to make progress even if low on memory!
        if (rc){
                spin_unlock(&checkpoints->obsoleted_entries_lock);
                dout("Here");
                goto error_unlock;
        } else {
                checkpoints->obsoleted_entries.size = 0;
                spin_unlock(&checkpoints->obsoleted_entries_lock);
        }

        if (sivfs_gc_blocked_entries_empty(checkpoints)){
                //Early exit
                mutex_unlock(&checkpoints->gc_blocked_entries_mutex);
                *did_gc_out = did_gc;
                return rc;
        }

        //Step 1: Read from some reccently published snapshot
        //Let this be MS for master snapshot
        sivfs_ts latest_snapshot =
                atomic64_read(&checkpoints->latest_checkpoint_published)
        ;

        //Step 2: Pull up all thread's reported checked out versions and
        //checked out snapshot versions, let these be the s_i
        struct sivfs_stack* tmp_checked_out_versions;
        rc = sivfs_new_stack(&tmp_checked_out_versions);
        if (rc)
                goto error_unlock;

        rc = sivfs_get_checked_out_versions(tmp_checked_out_versions);
        if (rc)
                goto error_free;

        if (SIVFS_DEBUG_GC) {
                size_t i = 0;
                for(i = 0; i < tmp_checked_out_versions->size; i++){
                        dout("Checked out version at GC time: %lu",
                        (unsigned long)tmp_checked_out_versions->values[i]);
                }
        }

        //At this point, we know that any thread will only use snapshot pages
        //that are visible in a snapshot s_i or [MS, infinity)

        //So, if we have a snapshot page that we know falls in none of these
        //checkpoints, it is collectable. To do so, we have a liveness [a,b) so
        //that any published snapshot that could contain it is in [a,b).
        //
        //If we extend b, we do so before publishing a new snapshot, so this
        //invariant still holds.

        //First, find any entries in gc_blocked_entries that are no longer
        //blocked
        //(If we have a SIVFS_INVALID_TS group, SIVFS_INVALID_TS always ends up
        //here exactly once)

        void** slot;
        struct radix_tree_iter iter;
        radix_tree_for_each_slot(
                slot,
                &checkpoints->gc_blocked_entries,
                &iter,
                0
        ){
                sivfs_ts blocked_ts = iter.index;
                bool still_blocked = false;
                //We use INVALID_TS to mark the group that we haven't assigned
                //to any other group yet
                //That group is always added to unblocked_ts
                if (blocked_ts == SIVFS_INVALID_TS){
                        still_blocked = false;
                } else {
                        //Check if it is in [MS, infinity)
                        if (blocked_ts >= latest_snapshot){
                                still_blocked = true;
                        }
                        //Check if blocked_ts is in checked out versions
                        size_t i = 0;
                        for(i = 0; i < tmp_checked_out_versions->size; i++){
                                if (
                                        (sivfs_ts)
                                        tmp_checked_out_versions->values[i]
                                        ==  blocked_ts
                                ){
                                        still_blocked = true;
                                }
                        }
                }

                //Can't free this group yet.
                if (still_blocked){
                        continue;
                }

                //Go through each of the pages and try to
                //either free them or recategorize.

                //At end, check if we were able to process all of them.
                //If we _were_, free the list and remove the entry.
                //Check if blocked_ts is in checked out versions
                sivfs_ts ts = blocked_ts;
                //Entries to reassign
                struct sivfs_stack* entries = *slot;

                //Reassign the pages, or free. We know that if reassigned they
                //will be assigned to a blocked group with a ts equal to one of
                //the si, which is guaranteed to not be in unblocked_ts.
                //That is, we are guaranted we won't push onto "pages" during
                //this operation.
                while(!sivfs_stack_empty(entries)){
                        sivfs_gc_entry entry =
                                (sivfs_gc_entry) sivfs_stack_peek(entries)
                        ;
                        rc = sivfs_gc_or_reassign_entry(
                                &did_gc,
                                state,
                                checkpoints,
                                &entry,
                                tmp_checked_out_versions,
                                latest_snapshot
                        );
                        //Can fail if the reassignment fails.
                        if (rc)
                                goto error_free;
                        //Success, do the actual pop
                        sivfs_stack_pop(entries);
                }
                //We reach here only iff pages becomes empty
                //Remove it
                radix_tree_delete(
                        &checkpoints->gc_blocked_entries,
                        ts
                );
                sivfs_put_stack(entries);
        }

out:
        *did_gc_out = did_gc;

        if (did_gc){
                //Made a snapshot => wake up log gc
                wake_up_interruptible(&sstate->make_checkpoint_wq);
        }

error_free:
        sivfs_put_stack(tmp_checked_out_versions);

error_unlock:
        mutex_unlock(&checkpoints->gc_blocked_entries_mutex);

error0:
        return rc;
}

int sivfs_gc_checkpoints_threadfn(void* ignored)
{
        int rc = 0;
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        bool exit_after_cleanup = false;

        dout("Starting gc_checkpoints thread");

        //Initialize state
        rc = sivfs_get_task_state(current);
        if (rc){
                goto error_spin0;
        }

        DEFINE_WAIT(wait);
        while(!kthread_should_stop()){
                bool did_gc = false;
                rc = sivfs_gc_checkpoints(&did_gc);
                if (rc) {
                        goto error_spin1;
                }

                //Wait for an SLA period and then go again
                msleep_interruptible(SIVFS_CHECKPOINT_GC_MDELAY);
                if (!did_gc){
                        //Do a longer sleep using gc_wq
                        prepare_to_wait(
                                &sstate->gc_wq,
                                &wait,
                                TASK_INTERRUPTIBLE
                        );
                        //Whether or not we succeed here doesn't matter
                        //we are just finishing off any work staged before we
                        //called prepare_to_wait
                        rc = sivfs_gc_checkpoints(&did_gc);
                        if (rc) {
                                finish_wait(&sstate->gc_wq, &wait);
                                goto error_spin1;
                        }
                        if (kthread_should_stop()){
                                finish_wait(&sstate->gc_wq, &wait);
                                goto out;
                        }
                        //This will NOT sleep if we received any signals since
                        //the last prepare_to_wait, and we checked
                        //kthread_should_stop since then.
                        schedule();
                        finish_wait(&sstate->gc_wq, &wait);
                }
        }

out:
        exit_after_cleanup = true;

error_spin1:
        //Clean up task state
        sivfs_put_task_state(current);

error_spin0:
        if (exit_after_cleanup){
                return rc;
        }
        //Need to spin on error
        dout("ERROR: stopping shapshot garbage collection");
        set_current_state(TASK_INTERRUPTIBLE);
        while(!kthread_should_stop()){
                schedule();
                set_current_state(TASK_INTERRUPTIBLE);
        }
        return rc;
}
