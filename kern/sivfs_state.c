#include <linux/string.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "sivfs_state.h"
#include "sivfs_shared_state.h"
#include "sivfs_radix_tree.h"
#include "sivfs_stack.h"
#include "sivfs.h"

static int sivfs_new_state(struct sivfs_state** new_state){
        int rc = 0;

        if (SIVFS_DEBUG_THREADS) dout("Initializing new sivfs_state");

        struct sivfs_state* state;
        state = kzalloc(sizeof(*state), GFP_KERNEL);
        if (!state){
                rc = -ENOMEM;
                goto error0;
        }

        //Initialization
        state->ts = SIVFS_INVALID_TS;
        atomic64_set(&state->checkpoint_ts, SIVFS_INVALID_TS);
        atomic64_set(&state->prior_checkpoint_ts, SIVFS_INVALID_TS);

        state->notifier.ops = &sivfs_mmuops;

        //One reference
        atomic64_set(&state->nref, 1);

        rc = sivfs_init_workingset(&state->workingset);
        if (rc)
                goto error_free;

        rc = sivfs_init_log_entry(&state->tmp_log_writeset);
        if (rc){
                goto error_free;
        }

        //Allocator metadata
        INIT_LIST_HEAD(&state->allocated_chunks);
        INIT_LIST_HEAD(&state->alloc_chunks_to_free);

out:
        *new_state = state;
        return rc;

error_free:
        kfree(state);

error0:
        return rc;
}

//Used for freeing the result of new_state
void sivfs_put_state(struct sivfs_state* state){
        if (SIVFS_DEBUG_THREADS) dout("Destroying sivfs_state");
        sivfs_destroy_workingset(&state->workingset);

        sivfs_destroy_log_entry(&state->tmp_log_writeset);

        if (state->notifier_registered){
                dout("Manually unregistering mmu notifier");
                struct mm_struct* mm = get_task_mm(state->t_owner);
                if (!mm){
                        dout("ERROR: Registered mmu notifier but destroying sivfs state!");
                } else {
                        mmu_notifier_unregister(&state->notifier, mm);
                        mmput(mm);
                }
        }

        //Clean up allocator metadata
        int rc = sivfs_allocation_on_commit(state, true, SIVFS_INVALID_TS);
        if (rc){
                //This should not occur, the only error cases are allocating
                //pginfo, but all the pginfo requested should already exist
                dout("ERROR: Error cleaning up allocator metadata.");
        }

        //Both of the following are relatively minor memory leaks, we clean up
        //the chunks themselves based on the pginfo tree in alloc info so when
        //the inode is destroyed the chunks will be destroyed regardless.
        if (!list_empty(&state->allocated_chunks)){
                dout("ERROR: Virtual memory leak: Destroying state with allocated chunks!");
        }
        if (!list_empty(&state->alloc_chunks_to_free)){
                dout("ERROR: Virtual memory leak: Destroying state with freed chunks!");
        }

        kfree(state);
}

int sivfs_get_task_state(struct task_struct* task){
        int rc = 0;
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        unsigned long taski = sivfs_task_to_radix_index(task);

        //If we want it to be possible for tasks to set states of other tasks,
        //then we need to wrap this whole method in threads_lock. Currently
        //not.
        //
        //Caller must have rcu read lock aquired.

        struct sivfs_state* state = radix_tree_lookup(
                &sstate->threads,
                taski
        );
        if (unlikely(!state)){
                //New object comes with nref == 1
                rc = sivfs_new_state(&state);
                if (rc)
                        goto error0;

                state->t_owner = task;

                spin_lock(&sstate->threads_lock);
                rc = radix_tree_insert(&sstate->threads, taski, state);
                spin_unlock(&sstate->threads_lock);
                if (rc == -EEXIST){
                        dout(
                        "Assertion error: task already had a sivfs_state!"
                        );
                }
                if (rc)
                        goto error_free;
        } else {
                //Increment ref count
                atomic64_inc(&state->nref);
        }

        if (SIVFS_DEBUG_THREADS) dout(
                "In get_task_state, refct = %p %zd",
                state,
                atomic64_read(&state->nref)
        );
out:
        return rc;

error_free:
        sivfs_put_state(state);

error0:
        return rc;
}

//Decrement the reference count on the state associated with current
void sivfs_put_task_state(struct task_struct* task){
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;
        unsigned long taski = sivfs_task_to_radix_index(task);

        //Again, if we want other tasks to set states of other tasks, this
        //whole method must be wrapped in a mutex
        //
        //Caller must have rcu read lock aquired.

        struct sivfs_state* state = radix_tree_lookup(&sstate->threads, taski);
        //state must be nonnull
        if (state == NULL){
                dout("Assertion error");
                return;
        }

        if (SIVFS_DEBUG_THREADS) dout(
                "In put_task_state, refct = %p %zd",
                state,
                atomic64_read(&state->nref)
        );

        if (atomic64_dec_and_test(&state->nref)){
                //Last reference, free to remove

                //Before we do, make sure its stats are tucked away somewhere
                sivfs_update_stats();

                spin_lock(&sstate->threads_lock);
                radix_tree_delete(&sstate->threads, taski);
                spin_unlock(&sstate->threads_lock);
                //And to erase state
                sivfs_put_state(state);

                //We updated what versions are checked out
                //=> wake up gc
                wake_up_interruptible(&sstate->gc_wq);
        }
}

struct page* sivfs_allocate_local_page(struct sivfs_state* state){
        return NULL;
}

//Outputs the snapshots checked out by each thread
//Caller should hold rcu read lock
int sivfs_get_checked_out_versions(struct sivfs_stack* out){
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        void** slot;
        struct radix_tree_iter iter;
        int rc = 0;
        //Lock
        spin_lock(&sstate->threads_lock);

        //Iterates over non-empty slots
        radix_tree_for_each_slot(
                slot,
                &sstate->threads,
                &iter,
                0
        ){
                struct sivfs_state* state = *slot;
                //Need to clflush to make sure the writes are obtained
                //(this flushes the line from other core's write-back buffers)
                clflush(&state->checkpoint_ts);
                clflush(&state->prior_checkpoint_ts);

                rc = sivfs_stack_push(out, (void*)atomic64_read(&state->checkpoint_ts));
                if (rc)
                        goto error_unlock;

                rc = sivfs_stack_push(out, (void*)atomic64_read(&state->prior_checkpoint_ts));
                if (rc)
                        goto error_unlock;
        }

error_unlock:
        //Unlock
        spin_unlock(&sstate->threads_lock);
        return rc;
}
