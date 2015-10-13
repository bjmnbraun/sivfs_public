#ifndef SIVFS_STATE_H
#define SIVFS_STATE_H

#include <linux/list.h>

//Represents the state of a single mmap
struct sivfs_state {
        int test;
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
        struct task_struct* task;

        //Member in sivfs_shared_state mmap_list
        struct list_head mmap_item;
};

int init_sivfs_state(struct sivfs_state* state);

//Only to be called on a sivfs_state* ptr that has been successfully init'ed
void destroy_sivfs_state(struct sivfs_state* state);

struct page* sivfs_allocate_local_page(struct sivfs_state* state);

#endif
