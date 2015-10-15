#ifndef SIVFS_CHECKPOINTS_H
#define SIVFS_CHECKPOINTS_H

//We make checkpoints of the log for recovery
//but we also format checkpoints so that they can be
//mapped into a thread's address space directly.

//Checkpoint pages are leveled trees
//Level 0 = leaf page
//Level 1 = page table (entries are pte_t ~ pointer-to-leaf-page)
//Level 2 = pmd (entries are pmd_t ~ pointer-to-page-table)
//Level 3 = pud (entries are pud_t ~ pointer-to-pmd)
//Level 4 = pgd (entries are pgd_t ~ pointer-to-pud)
//Level 5 and up have no special name and their entries are
//simply a pointer to a lower-leveled tree.

#include <linux/pfn.h>
#include <linux/mm.h>
#include <linux/radix-tree.h>

#include "sivfs_common.h"
#include "sivfs_types.h"
#include "sivfs_config.h"
#include "sivfs_stack.h"

#include "sivfs_inode_info.h"

//VA <-> PA <-> struct page
//    1      2
//1 is done with __pa / __va
//2 is done with the functions below
//
//Helper functions for messing with checkpoints, which are trees of pages with
//physical address pointers
//
//Highmemory struct page's don't have a static physical address
//This function is UNDEFINED for highmemory pages, the caller must check.
HEADER_INLINE phys_addr_t page_to_pa(struct page* page){
        return PFN_PHYS(page_to_pfn(page));
}

//New kernels publish this in page.h
#ifndef PHYS_PFN
HEADER_INLINE size_t PHYS_PFN(phys_addr_t pa){
        return (size_t)(pa >> PAGE_SHIFT);
}
#endif

#if SIVFS_COMPAT_LINUX < SIVFS_COMPAT_LINUX_4_6
//page_ref.h new header in 4_6 has a lot of convenient functions like this:
//This function still works in older versions of linux, though.
HEADER_INLINE int page_ref_count(struct page *page){
        return atomic_read(&page->_count);
}
#endif

//Does not translate pa == 0 to NULL.
HEADER_INLINE struct page* pa_to_page(phys_addr_t pa){
        return pfn_to_page(PHYS_PFN(pa));
}

//Some lifetime info about a page.
//Used for garbage collection.
struct sivfs_checkpoint_pginfo {
        //Liveness is [checkpoint_ts, obsoleted_ts)
        //obsoleted_ts is a checkpoint ts.
        sivfs_ts checkpoint_ts;
        sivfs_ts obsoleted_ts;
        //Level in the checkpoint, see SIVFS_CHECKPOINT_TOTAL_LEVELS
        size_t level;
        //Convenience bools, true iff level == 2 or 1, respectively
        bool is_pmd;
        bool is_pte;
        //Is a zero page (these are never reclaimed)
        bool is_zero_page;
        //Is a noprivate page (i.e. mapping == _checkpoint_page_noprivate_mapping)
        bool is_noprivate;
        //The allocation order of the page (usually 0 but larger for noprivate
        //hacks)
        unsigned int order;
};

//Hack to label pages as sivfs checkpoint pages or not
extern struct address_space _checkpoint_page_mapping;
extern struct address_space _checkpoint_page_noprivate_mapping;
HEADER_INLINE bool sivfs_is_checkpoint_page(struct page* page){
        return page == ZERO_PAGE(0)
                || page->mapping == &_checkpoint_page_mapping
                || page->mapping == &_checkpoint_page_noprivate_mapping;
}

struct sivfs_checkpoint_pginfo* sivfs_checkpoint_page_to_pginfo_noprivate(
        struct page* page
);

HEADER_INLINE struct sivfs_checkpoint_pginfo* sivfs_checkpoint_page_to_pginfo(
        struct page* page
){
        if (page->mapping == &_checkpoint_page_mapping){
                //Type pun the private field to a sivfs_snasphot_pginfo
                //return (struct sivfs_checkpoint_pginfo*)(&page->private);

                //Type pun the second double word (slub information) to a pginfo
                //This does not appear to work.
                //return (struct sivfs_checkpoint_pginfo*)(&page->index);

                //Type pun the third double word?
                //return (struct sivfs_checkpoint_pginfo*)(page->private);

                //Good old, honest, pointer-to-metadata... Slow but it works
                //Look back into type punning at a later date.
                return (struct sivfs_checkpoint_pginfo*)(page->private);
        } else {
                //Workaround for _noprivate pages
                return sivfs_checkpoint_page_to_pginfo_noprivate(page);
        }
}

//zero pages are not reclaimed in the usual way and should never be written to
HEADER_INLINE bool sivfs_is_zero_page(struct page* page){
        return page == ZERO_PAGE(0) || sivfs_checkpoint_page_to_pginfo(page)->is_zero_page;
}

int sivfs_new_checkpoint_page(
        struct page** page_out,
        sivfs_ts checkpoint_ts,
        size_t level
);
void sivfs_put_checkpoint_page(struct page* page);

void sivfs_mk_zero_checkpoint_page(void* mem, size_t level);

//Double-read-protected way to access the latest checkpoint
//read ts before and after. If it is INVALID_TS, then ts is dirty
//and a re-read of checkpoint_root is needed.
struct sivfs_latest_checkpoint{
        struct inode* checkpoint;
        atomic64_t ts;
};

//Number of levels in the tree (note - counts the leaf level. )
#define SIVFS_CHECKPOINT_TOTAL_LEVELS \
        (SIVFS_CHECKPOINT_INO_LEVELS + SIVFS_CHECKPOINT_FOFFSET_LEVELS)

#define SIVFS_CHECKPOINT_MIN_INO_LEVEL SIVFS_CHECKPOINT_FOFFSET_LEVELS

struct sivfs_checkpoints {
        //The latest published checkpoint
        //The checkpoint contains all changes up to and including the TS at time
        //ts.
        //
        //Need to use double-read-protect. SIVFS_INVALID_TS indicates a dirty
        //read, it does not mean that there is no checkpoint!
        struct sivfs_latest_checkpoint latest_checkpoint;

        //For convenience, this is updated after publishing a checkpoint.
        //So, this is guaranteed to always be <= the timestamp of the latest
        //published checkpoint.
        //This never reads SIVFS_INVALID_TS, so it is preferred to read from
        //this over latest_checkpoint.ts if you only need the timestamp and can
        //tolerate that this lags behind the published commit.
        atomic64_t latest_checkpoint_published;

        struct mutex checkpoints_reverse_mutex;
        //a datastructure to quickly look up an old checkpoint by
        //timestamp, we should return a root for that timestep
        //associates a timestamp with a struct inode*
        //
        //We map the NEGATIVE timestamp, so that a forward
        //iteration over this goes back in time
        //
        //Iterations and insertions both need to acquire
        //checkpoints_reverse_mutex.
        //
        //TODO replace with rb_tree, as it is sparse.
        struct radix_tree_root checkpoints_reverse;

        //Lock for obsoleted_entries
        spinlock_t obsoleted_entries_lock;
        //Vector of pages obsoleted as part of checkpoint generation.
        //Periodically drained by the checkpoint gc, the pages will be freed
        //or recategorized into gc_blocked_pages.
        struct sivfs_stack obsoleted_entries;

        //Lock for gc_blocked_entries
        struct mutex gc_blocked_entries_mutex;
        //This tree associates various checkpoint timestamps
        //that were checked out by some thread at some point
        //and are blocking the GC of some objects with those objects
        //
        //Does not use reverse indexing like checkpoints_reverse.
        //
        //Keys; timesamps
        //Values: struct sivfs_stack*s containing sivfs_gc_entry's
        struct radix_tree_root gc_blocked_entries;
};

#define SIVFS_CHECKPOINT_NODE_SHIFT 9
#define SIVFS_CHECKPOINT_CHILDREN_PER_NODE (PAGE_SIZE / sizeof(void*))
#define SIVFS_CHECKPOINT_NODE_MASK (SIVFS_CHECKPOINT_CHILDREN_PER_NODE - 1)

struct sivfs_checkpoint_iter {
        size_t offsets [SIVFS_CHECKPOINT_TOTAL_LEVELS-1];
        //The current index into offsets for the iteration
        size_t cur_i;
};

//Starts an iterator for traversing a checkpoint to a file_ino with file_offset
//Does bounds checking.
HEADER_INLINE int sivfs_checkpoint_iter_init(
        struct sivfs_checkpoint_iter* iter,
        unsigned long file_ino,
        size_t file_offset
){
        int rc = 0;
        //Zero for safety
        memset(iter, 0, sizeof(*iter));

        //Convert file_offset to an offset in pages, since the leaves of the
        //tree are whole pages
        size_t file_offset_pg = file_offset / PAGE_SIZE;

        //Some static checks that don't compile into real code:
        _Static_assert(
                (SIVFS_CHECKPOINT_TOTAL_LEVELS >= 1)
                , "SIVFS_CHECKPOINT_TOTAL_LEVELS must be >= 1"
        );
        _Static_assert(
                (
                        SIVFS_CHECKPOINT_CHILDREN_PER_NODE &
                        (SIVFS_CHECKPOINT_CHILDREN_PER_NODE - 1)
                ) == 0 &&
                SIVFS_CHECKPOINT_CHILDREN_PER_NODE > 0
        , "SIVFS_CHECKPOINT_CHILDREN_PER_NODE not a POT"
        );
        _Static_assert(
                (1ULL << SIVFS_CHECKPOINT_NODE_SHIFT) ==
                SIVFS_CHECKPOINT_CHILDREN_PER_NODE
        , "SIVFS_CHECKPOINT_NODE_SHIFT and CHILDREN_PER_NODE disagree"
        );

        size_t _cur_i = 0;

        int level;
        for(level = SIVFS_CHECKPOINT_TOTAL_LEVELS - 1;
                level >= SIVFS_CHECKPOINT_MIN_INO_LEVEL;
                level--
        ){
                //Sublevels is levels into the ino selection levels
                int sublevel = level - SIVFS_CHECKPOINT_MIN_INO_LEVEL;
                size_t shift = sublevel * SIVFS_CHECKPOINT_NODE_SHIFT;
                if (sublevel == SIVFS_CHECKPOINT_INO_LEVELS - 1){
                        if (
                                (file_ino >> shift)
                                >= SIVFS_CHECKPOINT_CHILDREN_PER_NODE
                        ){
                               dout(
                                       "file_ino is too large: %lu",
                                       file_ino
                               );
                               rc = -EINVAL;
                               goto error0;
                        }
                }
                //Emit the offset
                iter->offsets[_cur_i] = (file_ino >> shift) & SIVFS_CHECKPOINT_NODE_MASK;
                _cur_i++;
        }
        for( ; level >= 1; level--){
                //Sublevels is levels into the tables, i.e. 0 is page table
                int sublevel = level - 1;
                size_t shift = sublevel * SIVFS_CHECKPOINT_NODE_SHIFT;
                if (level == SIVFS_CHECKPOINT_FOFFSET_LEVELS - 1){
                        if (
                                (file_offset_pg >> shift)
                                >= SIVFS_CHECKPOINT_CHILDREN_PER_NODE
                        ){
                               dout(
                                       "file_offset is too large: %lu",
                                       file_offset
                               );
                               rc = -EINVAL;
                               goto error0;
                        }
                }
                iter->offsets[_cur_i] = (file_offset_pg >> shift) & SIVFS_CHECKPOINT_NODE_MASK;
                _cur_i++;
        }
        //Finally, level 0 refers to the leaf nodes - no offset for level 0
        //since we have already arrived at the bottom of the tree

error0:
        return rc;
}

//Advances an iterator. Modifies the iterator in place.
HEADER_INLINE void sivfs_checkpoint_iter_advance(
        struct sivfs_checkpoint_iter* iter
){
        iter->cur_i++;
}

//Returns the offset at the current location in the iterator for the next
//child
HEADER_INLINE size_t sivfs_checkpoint_iter_next_offset(
        struct sivfs_checkpoint_iter* iter
){
        return iter->offsets[iter->cur_i];
}

int sivfs_init_checkpoints(struct sivfs_checkpoints* checkpoints);

void sivfs_destroy_checkpoints(struct sivfs_checkpoints* checkpoints);

void sivfs_checkpoint_lookup(
        struct inode** checkpoint_out,
        struct sivfs_checkpoints* checkpoints,
        sivfs_ts desired_ts
);

//sivfs_checkpoints_insert has two phases, first _sivfs_checkpoint_insert
//then _sivfs_checkpoint_insert_finish to commit the insertion.
//If the insertion needs to be aborted, call _sivfs_checkpoints_remove instead
//of _finish.
int _sivfs_checkpoints_insert(
        struct sivfs_checkpoints* checkpoints,
        struct inode* checkpoint
);

//Undoes a call to _sivfs_checkpoints_insert, prior to finalizing
//the insertion with _finish. Used to abort a speculative insertion.
void _sivfs_checkpoints_remove(
        struct sivfs_checkpoints* checkpoints,
        struct inode* checkpoint
);

void _sivfs_checkpoints_insert_finish(
        struct sivfs_checkpoints* checkpoints,
        struct inode* checkpoint
);

HEADER_INLINE int sivfs_checkpoints_insert(
        struct sivfs_checkpoints* checkpoints,
        struct inode* checkpoint
){
        int rc = 0;

        rc = _sivfs_checkpoints_insert(
                checkpoints,
                checkpoint
        );

        if (rc){
                dout("Here");
                goto error0;
        }

        _sivfs_checkpoints_insert_finish(
                checkpoints,
                checkpoint
        );

error0:
        return rc;
}

//Consider moving this private
int sivfs_checkpoint_traverse(
        struct page** page_out,
        struct page* checkpoint_root,
        unsigned long file_ino,
        size_t file_offset
);

//Looks up a page in a checkpoint file_offset bytes into file with backing file
//ino file_ino
//rcu_read lock must be held by caller and for the whole time checkpoint_page is
//used
//checkpoint_ts returns the ts of the checkpoint
//desired_ts is a timestamp for which the checkpoint is desired
//the returned checkpoint page will be at least as old as desired_ts, i.e. it
//is guaranteed to not be newer
//
//If there is no prior checkpoint, checkpoint_ts is set to 0 and checkpoint_page is
//set to NULL
HEADER_INLINE int sivfs_checkpoint_lookup_page(
        struct page** checkpoint_page_out,
        sivfs_ts* checkpoint_ts_out,
        struct sivfs_checkpoints* checkpoints,
        sivfs_ts desired_ts,
        unsigned long file_ino,
        size_t file_offset
) {
        int rc = 0;

        struct inode* checkpoint;
        sivfs_checkpoint_lookup(&checkpoint, checkpoints, desired_ts);

        if (!checkpoint){
                //This should never happen, since we insert an initial checkpoint
                //at time 0
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;
        if (!cpinfo){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        struct page* checkpoint_page;
        rc = sivfs_checkpoint_traverse(
                &checkpoint_page,
                cpinfo->cp_root,
                file_ino,
                file_offset
        );
        if (rc)
                goto error0;

out:
        *checkpoint_page_out = checkpoint_page;
        *checkpoint_ts_out = cpinfo->cp_ts;

error0:
        return rc;
}

//Read a range from a checkpoint into buffer mem
int sivfs_read_checkpoint(
        void* mem,
        size_t mem_size,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset
);

//Same return codes as sivfs_fault.
int sivfs_lock_cp_page(
        struct page** page_out,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset,
        unsigned int flags
);

//Same return codes as sivfs_fault.
int sivfs_get_cp_page(
        struct page** page_out,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset
);

//Lookup a checkpoint page at a specific level above file_offset
//Options are 0 = leaf page, 1 = page-of-ptes, 2 = page-of-pmds, 3 = page-of-puds
//etc
//
//The page's refct is unchanged and the page is not locked
//
//Returns standard error codes (unlike get_cp_page or lock_cp_page above)
int sivfs_lookup_cp_page_at_level(
        struct page** page_out,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset,
        int level
);


//gc_blocked_entries data type

typedef unsigned long sivfs_gc_entry;

//Use the low three bits of the pointers to encode type.
//This is because all pointers should be 64-bit (=8 byte) aligned.
#define SIVFS_GC_ENTRY_PAGE (1UL << 1)
#define SIVFS_GC_ENTRY_INODE (1UL << 2)
//Update this to contain the above flags
#define SIVFS_GC_ENTRY_FLAGS (0x7UL)

HEADER_INLINE int sivfs_gc_entry_to_type(unsigned long* type_out, sivfs_gc_entry* entry){
        //Set-contains check
        _Static_assert(
                (SIVFS_GC_ENTRY_FLAGS | SIVFS_GC_ENTRY_PAGE | SIVFS_GC_ENTRY_INODE)
                == SIVFS_GC_ENTRY_FLAGS,
                "SIVFS_GC_ENTRY_FLAGS must contain all type flags"
        );
        //Type-punnable-to-void* check
        _Static_assert(
                 sizeof(sivfs_gc_entry) <= sizeof(void*),
                "sivfs_gc_entry must be type-punnable to void*"
        );
        //sivfs_gc_entry must be type-punnable to a void* for use in
        //radix tree
        unsigned long flags = (*entry) & SIVFS_GC_ENTRY_FLAGS;
        switch(flags){
        case SIVFS_GC_ENTRY_PAGE:
                *type_out = SIVFS_GC_ENTRY_PAGE;
                return 0;
        case SIVFS_GC_ENTRY_INODE:
                *type_out = SIVFS_GC_ENTRY_INODE;
                return 0;
        }
        //Otherwise
        return -EINVAL;
}

HEADER_INLINE int sivfs_gc_make_entry_page(sivfs_gc_entry* entry, struct page* page){
        if ((unsigned long)page & SIVFS_GC_ENTRY_FLAGS){
                dout("Here %p", page);
                return -EINVAL;
        }
        *entry = ((unsigned long)page) | SIVFS_GC_ENTRY_PAGE;
        return 0;
}

HEADER_INLINE int sivfs_gc_make_entry_inode(sivfs_gc_entry* entry, struct inode* inode){
        if ((unsigned long)inode & SIVFS_GC_ENTRY_FLAGS){
                dout("Here");
                return -EINVAL;
        }
        *entry = ((unsigned long)inode) | SIVFS_GC_ENTRY_INODE;
        return 0;
}

HEADER_INLINE struct page* sivfs_gc_entry_to_page(sivfs_gc_entry* entry){
        return (struct page*)(*entry & ~SIVFS_GC_ENTRY_FLAGS);
}

HEADER_INLINE struct inode* sivfs_gc_entry_to_inode(sivfs_gc_entry* entry){
        return (struct inode*)(*entry & ~SIVFS_GC_ENTRY_FLAGS);
}

//State can be null
void sivfs_gc_free_entry(
        sivfs_gc_entry* entry,
        struct sivfs_state* state
);

#endif //ifndef SIVFS_CHECKPOINTS_H
