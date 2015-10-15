#ifndef SIVFS_INODE_INFO_H
#define SIVFS_INODE_INFO_H

#include <linux/mm.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "sivfs_common.h"
#include "sivfs_types.h"
#include "sivfs_workingset.h"

#include "alloc/sivfs_allocation.h"

struct sivfs_state;

//Special values for inode_info->refcount
#define SIVFS_UNREFERENCED -1000
//What this means is that you can mmap an anon inode at most this many times
//No reasonable program should get anywhere close to this
#define SIVFS_IINFO_MAX_REFCOUNT 100

//Information associated with an inode
//(associated with both anon and non-inode)
//inodes cannot have per-process information in them, as they are cleaned up
//potentially independently of process cleanup. We store per-process
//information in the vma_info.
struct sivfs_inode_info {
//Anon-inode fields
        //For an anonymous (invisible) inode with a backing_inode,
        struct inode* backing_inode;
        //The state this anon inode is part of
        struct sivfs_state* state;

        //Refcount on the anon inode and / or corresponding pseudo-dentry
        //See sivfs_refct.h for proper use
        int refcount;

        //In some rare cases, we need to invalidate all workingset pages.
        // MMU code, i.e. zap_pte_range, can be hopelessly slow since
        // empty ptes are kept around (indefinitely?)
        //
        // An alternative, iterating over the i_mapping->page_tree for entries
        // is better, but has unsatisfying performance when only one or two
        // pages are mapped in (this becomes a problem only when using a
        // page cache size of 0, i.e. invalidate at end of txn.)
        //
        // For the specific case where we are invalidating the entire
        // workingset at the end of each transaction, we can use a helper DS
        // which is just the ranges mapped in during this transaction.
        // This may contain duplicate / overlapping ranges.
        //
        struct sivfs_workingset_ranges anon_ranges;

        //XXX The page cached doesn't work well enough for us ; inserting
        //exceptional entries in page_tree seems to cause instability. So, we
        //have to manage two trees - the page_tree only contains mapped in
        //pages and this contains checkpoint pages.
        struct radix_tree_root checkpoint_pages;
        bool checkpoint_checked_out;

//Non-anon-inode, i.e. backing inode, fields
        //Defined for non-anon inodes, points to a dentry that can be used to
        //access underlying file direectly
        //XXX is this valid?
        struct dentry* commit_dentry;

        struct sivfs_allocator_info* alloc_info;

//Checkpoint inode fields
        //Actual checkpoint tree
        struct page* cp_root;
        //ts
        sivfs_ts cp_ts;
        //ts of obsoleting checkpoint, i.e. next checkpoint
        sivfs_ts obsoleted_ts;
};

HEADER_INLINE struct sivfs_inode_info* sivfs_inode_to_iinfo(
        struct inode* inode
){
        return inode->i_private;
}

HEADER_INLINE struct sivfs_inode_info* sivfs_vma_to_iinfo(
        struct vm_area_struct* vma
){
        return sivfs_inode_to_iinfo(vma->vm_file->f_inode);
}

HEADER_INLINE struct inode* sivfs_vma_to_backing_inode(
        struct vm_area_struct* vma
){
        struct sivfs_inode_info* iinfo = sivfs_vma_to_iinfo(vma);
        //Get the backing (non-anonymous) inode
        return iinfo->backing_inode;
}

HEADER_INLINE struct sivfs_inode_info* sivfs_vma_to_backing_iinfo(
        struct vm_area_struct* vma
){
        //Get iinfo of the backing (non-anonymous) inode
        return sivfs_inode_to_iinfo(sivfs_vma_to_backing_inode(vma));
}

HEADER_INLINE int sivfs_iinfo_get_alloc_info(
        struct sivfs_allocator_info** ainfo_out,
        struct sivfs_inode_info* iinfo
) {
        int rc = 0;
        struct sivfs_allocator_info* toRet = iinfo->alloc_info;
        if (!toRet){
                rc = sivfs_new_allocator_info(&toRet);
                if (rc){
                        dout("Here");
                        goto error0;
                }

                //TODO need to check if we have recovery information in the
                //iinfo, if so DON'T insert this wilderness chunk and rely on
                //the recovery process to set up the allocator info.
                rc = sivfs_alloc_info_recover(toRet, NULL);
                if (rc){
                        dout("Here");
                        sivfs_put_allocator_info(toRet);
                        goto error0;
                }

                //Need a compare and swap here
                struct sivfs_allocator_info* race_winner;
                race_winner = cmpxchg(&iinfo->alloc_info, NULL, toRet);
                if (race_winner){
                        //If we get here, then someone raced with us and won
                        sivfs_put_allocator_info(toRet);
                        toRet = race_winner;
                }
        }
        *ainfo_out = toRet;
error0:
        return rc;
}


HEADER_INLINE int sivfs_new_iinfo(struct sivfs_inode_info** new_iinfo){
        int rc = 0;

        struct sivfs_inode_info* iinfo;
        iinfo = kzalloc(sizeof(*iinfo), GFP_KERNEL);

        if (!iinfo){
                rc = -ENOMEM;
                goto error0;
        }

        INIT_RADIX_TREE(&iinfo->checkpoint_pages, GFP_KERNEL);
        iinfo->obsoleted_ts = SIVFS_INVALID_TS;

out:
        *new_iinfo = iinfo;
        return rc;

error0:
        return rc;
}

//Used for freeing the result of new_iinfo
HEADER_INLINE void sivfs_put_iinfo(struct sivfs_inode_info* iinfo){
        if (unlikely(ZERO_OR_NULL_PTR(iinfo))){
                return;
        }

        sivfs_radix_tree_clear(&iinfo->checkpoint_pages);
        if (!sivfs_radix_tree_empty(&iinfo->checkpoint_pages)){
                dout("ERROR: Iinfo still had checkpoint page tree");
        }

        if (iinfo->alloc_info){
                sivfs_put_allocator_info(iinfo->alloc_info);
        }

        sivfs_workingset_ranges_destroy(&iinfo->anon_ranges);
        kfree(iinfo);
}

//Safe refcount manipulation.
HEADER_INLINE int sivfs_iinfo_refct_inc(
        struct sivfs_inode_info* iinfo
){
        if (iinfo->refcount == SIVFS_IINFO_MAX_REFCOUNT){
                //Can't increment any more, so return -EINVAL
                //Doesn't require any sort of debug message
                return -EINVAL;
        }
        if (iinfo->refcount < 0 || iinfo->refcount > SIVFS_IINFO_MAX_REFCOUNT){
                dout("ERROR: Bad refcount in sivfs_iinfo_refct_inc %d", iinfo->refcount);
                return -EINVAL;
        }
        iinfo->refcount++;
        return 0;
}

#endif
