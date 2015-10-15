#ifndef SIVFS_STATE_H
#define SIVFS_STATE_H

#include <linux/mm.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mmu_notifier.h>

#include "sivfs_shared_state.h"
#include "sivfs_types.h"
#include "sivfs_common.h"
#include "sivfs_workingset.h"
#include "sivfs_log.h"
#include "sivfs_stats.h"

//#include "sivfs_stack.h"
struct sivfs_stack;

//Represents the state of a task
//Only tasks with a matching task_struct pointer can call methods on this
//mmap
//
//Note that this is ONLY used for concurrency control (to ensure that two
//threads don't try to simultaneously perform an operation on the mmap.) It
//is NOT used to ensure access control. Access control is handled in the
//sense that a thread has to have access to the mmap context, which is only
//passed down through forking / threading. If a task struct ends up being
//reused, and the new reuser has legitimate access that's OK they can go
//ahead to work with the mmap. Still, only one task can have a given task
//struct at a time so we do solve the concurrency issue.
struct sivfs_state {
        //Reference count
        atomic64_t nref;

        //Task that owns this state
        struct task_struct* t_owner;

        //The snapshot timestamp of the current transaction.
        //Is SIVFS_INVALID_TS if not currently in a transaction
        sivfs_ts ts;

        //A checkpoint checked out by by this transaction
        struct inode* checkpoint;

        //Is SIVFS_INVALID_TS if not currently in a transaction
        atomic64_t checkpoint_ts;
        //The prior value of checkpoint_ts, only differs from checkpoint_ts during
        //the phase where we are checking out a new checkpoint
        atomic64_t prior_checkpoint_ts;

        //Is true iff we are in direct access mode, i.e. the thread
        //is allowed to modify the latest checkpoint
        //(checkpoint_ts) directly.
        bool direct_access;  

        struct sivfs_workingset workingset;

        //Temporary log entry used for storage during commit
        struct sivfs_log_entry tmp_log_writeset;

        //Stats. A current version, stats, and a stats_last_snapshot used to
        //updating a global stats snapshot.
        struct sivfs_stats stats;
        struct sivfs_stats stats_last_snapshot;

        //Workaround allowing us to COW checked-out checkpoint pages in
        //VM_SHARED mappings
        struct page* cp_page_to_cow;

        struct mmu_notifier notifier;
        bool notifier_registered;

        //Allocated chunks this transaction
        struct list_head allocated_chunks;

        //Chunks to free on commit
        struct list_head alloc_chunks_to_free;

        //Did the transaction do some action that requires us to abort?
        bool should_abort;
};

HEADER_INLINE bool sivfs_in_txn(struct sivfs_state* state){
        return state->ts != SIVFS_INVALID_TS;
}

HEADER_INLINE unsigned long sivfs_task_to_radix_index(struct task_struct* task){
        //Using task->pid doesn't work because, apparently, during process
        //death a task struct's pid may change (?) before some of the file
        //system cleanup is called. However, task struct pointers can be used
        //to still identifier the owner...
        //dout("Task %p had pid %d", task, task->pid);
        return (unsigned long) task;
}


//Get the sivfs_state associated with task
//Will create the state if it does not exist yet
//Either way, the reference count on the state is incremented
int sivfs_get_task_state(struct task_struct* task);

//Decrement the reference count on the state associated with current
void sivfs_put_task_state(struct task_struct* task);

//Outputs 2*N entries where N is the number of states
//First entry is the checked out snapshot_ts
//Second entry is the transaction timestamp
//And so on.
//Caller should hold rcu read lock
int sivfs_get_checked_out_versions(struct sivfs_stack* out);

HEADER_INLINE struct sivfs_state* sivfs_task_to_state(struct task_struct* task){
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;
        unsigned long taski = sivfs_task_to_radix_index(task);

        return radix_tree_lookup(&sstate->threads, taski);
}

struct page* sivfs_allocate_local_page(struct sivfs_state* state);

#endif
