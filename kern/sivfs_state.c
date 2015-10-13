#include <linux/string.h>

#include "sivfs_state.h"
#include "sivfs_allocation.h"

int init_sivfs_state(struct sivfs_state* state){
        //Init should begin with a zeroing for safety
        memset(state, 0, sizeof(*state));
        INIT_LIST_HEAD(&state->mmap_item);
        return 0;
}

void destroy_sivfs_state(struct sivfs_state* state){
}

struct page* sivfs_allocate_local_page(struct sivfs_state* state){
        return NULL;
}
