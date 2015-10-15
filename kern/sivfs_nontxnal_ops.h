#ifndef _SIVFS_NONTXNAL_OPS_H
#define _SIVFS_NONTXNAL_OPS_H

#include <linux/fs.h>

struct sivfs_state;

//These operations are "transactions" within themselves
//but cannot be used within a larger transaction, because we need to match
//some standard UNIX behavior such as a file truncate. These operations never
//abort.
//
//Each one of them has a side effect of updating the currently checked out
//version, because we need to update to a version that sees the outcome of the
//operation.
//
//Transactional versions of some of these operations may be possible, but for
//now there is no reason to implement that.

//Performs a commit that zeroes out after the truncation offset
//Note that the caller should also flush workingset pages in the truncated
//range, this is not currently done.
int sivfs_ftruncate(
        struct sivfs_state* state,
        struct inode* backing_inode,
        loff_t old_size,
        loff_t offset
);

#endif
