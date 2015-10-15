#ifndef SIVFS_MAKE_CHECKPOINT_H
#define SIVFS_MAKE_CHECKPOINT_H

#include "sivfs_common.h"

//Continuously makes snaphots using the workqueue and timeouts
int sivfs_make_checkpoint_threadfn(void* ignored);

//Direct-access to checkpoint sometimes needs to modify an existing
//checkpoint by inserting a zeroed out page (but not a zero page, since
//you can't write to zero pages) so this call does that.
int sivfs_prepare_direct_access_page(
        struct sivfs_state* state,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset
);

#endif
