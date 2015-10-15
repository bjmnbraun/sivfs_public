#ifndef SIVFS_GC_CHECKPOINTS_H
#define SIVFS_GC_CHECKPOINTS_H

struct sivfs_stack;

//Called once for every new checkpoint to mark the old checkpoint and any of
//its overwritten pages obsolete.
int sivfs_gc_mark_pages_obsoleted(
        struct sivfs_stack* obsoleted_pages,
        struct inode* obsoleted_checkpoint
);

//Continuously garbage collects unused snaphots
int sivfs_gc_checkpoints_threadfn(void* ignored);

#endif
