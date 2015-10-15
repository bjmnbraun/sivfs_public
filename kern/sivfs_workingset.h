#ifndef SIVFS_WORKINGSET_H
#define SIVFS_WORKINGSET_H

#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/buffer_head.h>

#include "sivfs_radix_tree.h"
#include "sivfs_config.h"

struct sivfs_log_entry;
struct sivfs_state;

struct sivfs_workingset_range {
        loff_t start;
        size_t size;
};

//vector reimplementation. We do this so that entries can be a sequential
//array of workingset_ranges, not pointers-to-workingset-ranges
struct sivfs_workingset_ranges {
        struct sivfs_workingset_range* entries;
        size_t size;
        size_t capacity;
};

HEADER_INLINE int sivfs_workingset_ranges_reserve(
        struct sivfs_workingset_ranges* ws,
        size_t new_capacity
){
        int rc = 0;
        if (new_capacity >= ws->capacity){
                //reallocate
                new_capacity = max3_size_t(
                        ws->capacity * 2,
                        4,
                        new_capacity
                );
                struct sivfs_workingset_range* newalloc;
                newalloc = kzalloc(
                        new_capacity * sizeof(*ws->entries),
                        GFP_KERNEL
                );

                if (!newalloc){
                        rc = -ENOMEM;
                        goto error0;
                }
                struct sivfs_workingset_range* old_entries = ws->entries;
                memcpy(
                        newalloc,
                        old_entries,
                        sizeof(*old_entries) * ws->size
                );
                kfree(old_entries);
                ws->capacity = new_capacity;
                ws->entries = newalloc;
        }

//out:
error0:
        return rc;
}

HEADER_INLINE int sivfs_workingset_ranges_add(
        struct sivfs_workingset_ranges* ws,
        const struct sivfs_workingset_range toAdd
){
        int rc = 0;
//Only if we clear the workingset every txn can we use
//this kind of datastructure to track workingset ranges.
        if (!SIVFS_TRACK_CLEAR_WORKINGSET_EVERY_TXN){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }
        size_t new_size = ws->size + 1;
        rc = sivfs_workingset_ranges_reserve(ws, new_size);
        if (rc)
                goto error0;
        //Set new size and do struct assignment
        ws->entries[ws->size] = toAdd;
        ws->size = new_size;

error0:
        return rc;
}

HEADER_INLINE void sivfs_workingset_ranges_destroy(
        struct sivfs_workingset_ranges* ws
){
        kfree(ws->entries);
}

struct sivfs_workingset_ranges_iter {
        size_t i;
        struct sivfs_workingset_range* value;
};

HEADER_INLINE void sivfs_workingset_iterate(
        struct sivfs_workingset_ranges* ranges,
        struct sivfs_workingset_ranges_iter* iter
){
        //No nullpointer can result from this as long as iter is non-null
        iter->value = ranges->entries + iter->i;
}

#define sivfs_workingset_ranges_for_each(ranges, iter) \
        for( \
                (iter)->i = 0, sivfs_workingset_iterate(ranges, iter); \
                (iter)->i < (ranges)->size; \
                (iter)->i++, sivfs_workingset_iterate(ranges, iter) \
        )


struct sivfs_workingset {
        //Maps backing inos to anon inodes
        struct radix_tree_root anon_inodes;
        struct radix_tree_root anon_dentries;
};

HEADER_INLINE int sivfs_init_workingset(struct sivfs_workingset* workingset){
        //Init should begin with a zeroing for safety
        memset(workingset, 0, sizeof(*workingset));

        INIT_RADIX_TREE(&workingset->anon_inodes, GFP_KERNEL);
        INIT_RADIX_TREE(&workingset->anon_dentries, GFP_KERNEL);
        return 0;
}
HEADER_INLINE void sivfs_destroy_workingset(
        struct sivfs_workingset* workingset
){
        //assert that the workingset is empty
        if (!sivfs_radix_tree_empty(&workingset->anon_inodes)){
                dout("Destroying workingset with open anon files!");
        }
        if (!sivfs_radix_tree_empty(&workingset->anon_dentries)){
                dout("Destroying workingset with open dentries!");
        }
}

//Gets an anonymous inode for backing file_inode, creating if necessary.
//
//Anonymous inodes and dentries are reference counted. This is a bit of a
//challenge, since the d_real and d_select_inode calls don't properly reference
//count - each of these calls expects a return that simply persists,
//indefinitely?
//
//Luckily, these calls are used in a small number of places, mostly in a
//context where there is an open file handle that we can use to keep a
//reference on the anon inode. So, we fake a reference counting system as
//follows:
//
// - d_real / d_select_inode create an inode / dentry as needed.
//   The created inode has a pointer to an inode_info that points to a state
//   the inode_info tracks how many open file references to the inode, and acts
//   as a reference count. The special refcount of UNREFERENCED is used until the first
//   file open. If the refcount falls to zero after being opened for the first
//   time, we deallocate and clear the pointer to the inode info
//
//   If that inode is then passed to open in the future (a brief scan of Linux
//   source code shows this should never happen, but just being safe) we return
//   an error code at that time by noticing that the inode_info pointer is
//   NULL.
//
//   An anon inode created but then never opened persists until the workingset
//   is cleared. Of course, this should never occur except for error cases and
//   is just a performance worry in the worst case.
//
// Set create_unopened when creating an inode but not opening it, otherwise we
// increment the refcount on the existing inode and return NULL if it is not
// exist.

//Flags can contain of these flags:
//Should we create one if not existant
#define SIVFS_CREATE 2
//Don't increment the refcount. A newly created inode will have refcount
//UNREFERENCED.
#define SIVFS_SKIP_REFCOUNT 4

int sivfs_get_anon_inode(
        struct dentry** dentry_out,
        struct sivfs_state* state,
        struct inode* backing_inode,
        int flags
);

// Decrements the refcount on an opened anon inode
void sivfs_put_anon_inode(
        struct sivfs_state* state,
        unsigned long backing_ino
);

//Should be called with rcu read lock held... TODO
HEADER_INLINE struct inode* sivfs_backing_ino_to_anon_inode(
        struct sivfs_workingset* ws,
        unsigned long backing_ino
){
        struct inode* toRet = radix_tree_lookup(&ws->anon_inodes, backing_ino);

        return toRet;
}

//Deprecated - use backing_ino_to_anon
HEADER_INLINE struct inode* sivfs_file_ino_to_anon_inode(
        struct sivfs_workingset* ws,
        unsigned long file_ino
){
        return sivfs_backing_ino_to_anon_inode(ws, file_ino);
}

//Invalidate workingset pages modified by "logent"
int sivfs_invalidate_workingset_log(
        struct sivfs_workingset* workingset,
        struct sivfs_log_entry* logent
);

//If options contains SIVFS_NO_INVALIDATE_CHECKPOINT, keeps checkpoint pages
//mapped in. This is useful for just cleaning up the pagecache without kicking
//out checkpoint pages.

#define SIVFS_NO_INVALIDATE_CPS 4

int sivfs_invalidate_workingset(
        struct sivfs_workingset* ws,
        int options
);

int sivfs_mark_clean_workingset(struct sivfs_workingset* workingset);

#define SIVFS_EVICT_CHECKPOINT 2

int sivfs_find_get_wspg(
        struct page** out_pg,
        struct sivfs_state* state,
        struct sivfs_workingset* ws,
        unsigned long file_ino,
        size_t offset,
        int flags
);

HEADER_INLINE void sivfs_put_wspg(
        struct page* page
){
        put_page(page);
}

#endif //ifndef SIVFS_WORKINGSET_H
