#include "sivfs_shared_state.h"

struct sivfs_shared_state sivfs_shared_state;

int init_sivfs_shared_state(struct sivfs_shared_state* sstate){
        //Init should begin with a zeroing for safety
        memset(sstate, 0, sizeof(*sstate));
        INIT_LIST_HEAD(&sstate->mmap_list);
        spin_lock_init(&sstate->mmap_list_lock);
        return 0;
}

