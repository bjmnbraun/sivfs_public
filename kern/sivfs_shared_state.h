#ifndef SIVFS_SHARED_STATE_H
#define SIVFS_SHARED_STATE_H

#include <linux/list.h>
#include <linux/spinlock.h>

#include "sivfs_state.h"

struct sivfs_shared_state {
        //list of struct sivfs_state*
        //Use kfree to free these
        struct list_head mmap_list;
        //Lock for above list
        spinlock_t mmap_list_lock;
};

//One global copy
extern struct sivfs_shared_state sivfs_shared_state;

int init_sivfs_shared_state(struct sivfs_shared_state* sstate);

#endif
