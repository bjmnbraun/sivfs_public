#ifndef SIVFS_RADIX_TREE_H
#define SIVFS_RADIX_TREE_H

#include <linux/radix-tree.h>
#include "sivfs_common.h"
#include "sivfs_config.h"

//Reserve a (definitely) not valid pointer as a tombstone entry
static void* const SIVFS_RADIX_TREE_TOMBSTONE =
        (void*)(((unsigned long)1)<<63 | RADIX_TREE_EXCEPTIONAL_ENTRY)
;

//Just adds a few methods to the radix_tree api
//Note that radix trees have significant restrictions on what values are stored
//  - NULL is never allowed
//  - In fact, the low two bits of "value" must be 0. This is the case for
//  storing 4-byte-aligned pointers, but you can run into trouble if you try to
//  store non-4-byte-aligned pointers or even just simple numbers.
//   Note that kmalloc is always at least 4 byte aligned.

HEADER_INLINE size_t sivfs_radix_tree_size(struct radix_tree_root* tree){
        size_t size = 0;
        void** slot;
        struct radix_tree_iter iter;

        //Iterates over non-empty slots
        radix_tree_for_each_slot(
                slot,
                tree,
                &iter,
                0
        ){
                size++;
        }


        return size;
}

HEADER_INLINE bool sivfs_radix_tree_empty(struct radix_tree_root* tree){
        void** slot;
        struct radix_tree_iter iter;

        //Iterates over non-empty slots
        radix_tree_for_each_slot(
                slot,
                tree,
                &iter,
                0
        ){
                //if (true) supresses info: ignoring unreachable code
                if (true) return false;
        }

        return true;
}

//Removes an element in a non-destructive fashion by placing a
//SIVFS_RADIX_TREE_TOMBSTONE at index. This is useful for sparse trees
//where we never do range queries and are repeatedly removing and adding the
//same elements because it avoids unnecessary deallocation
HEADER_INLINE void sivfs_radix_tree_tombstone(struct radix_tree_root* tree,
unsigned long index){
        void** slot = radix_tree_lookup_slot(tree, index);
        if (slot){
                //Oh whatever, GCC. This fails on -O0, which I use for perf
                //profiling sometimes.
                //radix_tree_replace_slot(slot, SIVFS_RADIX_TREE_TOMBSTONE);
                *slot = SIVFS_RADIX_TREE_TOMBSTONE;
        }
}

HEADER_INLINE void sivfs_radix_tree_clear(struct radix_tree_root* tree){
        unsigned long index;
        int i, nr;
        void** slot;
        struct radix_tree_iter iter;
        unsigned long indices[16];

        index = 0;
        //Acc to gmap_radix_tree_free, the stuff below should work.
        do {
                nr = 0;
                radix_tree_for_each_slot(
                                slot,
                                tree,
                                &iter,
                                0
                                )
                {
                        //We don't do anything with the element itself

                        indices[nr] = iter.index;
                        if (++nr == 16)
                                break;
                }
                for(i = 0; i < nr; i++){
                        index = indices[i];
                        radix_tree_delete(tree, index);
                }
        } while (nr > 0);
}

/*
HEADER_INLINE int sivfs_radix_tree_insert_not_exists(
        bool* inserted,
        void*** slot_out,
        struct radix_tree_root *root,
        unsigned long index,
        void* entry
){
        int rc = 0;

        void** slot = radix_tree_lookup_slot(root, index);
        if (unlikely(!slot)){
                rc = radix_tree_insert(root, index, entry);
                if (rc){
                        goto error0;
                }
                slot = radix_tree_lookup_slot(root, index);
                *inserted = true;
                goto out;
        }

        if (likely(!*slot)){
                radix_tree_replace_slot(slot, entry);
                *inserted = true;
        } else {
                *inserted = false;
        }

out:
        *slot_out = slot;

error0:
        return rc;
}
*/

//Sets an index to have some entry, if the the entry already exists and is
//unequal to entry, returns -EEXIST
HEADER_INLINE int sivfs_radix_tree_set(
        struct radix_tree_root *root,
        unsigned long index,
        void* entry
){
        void** slot = radix_tree_lookup_slot(root, index);
        if (!slot){
                return radix_tree_insert(root, index, entry);
        } else if (*slot == SIVFS_RADIX_TREE_TOMBSTONE){
                //Removing a tombstone is always safe
                radix_tree_delete(root, index);
                return radix_tree_insert(root, index, entry);
        } else if (*slot == entry){
                //Slot is already correct.
                return 0;
        } else {
                //Slot is incorrect
                return -EEXIST;
        }
}

#endif
