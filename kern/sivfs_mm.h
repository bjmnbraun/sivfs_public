#ifndef SIVFS_MM_H
#define SIVFS_MM_H

//Helper routines for manipulating page table maps

#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/pfn.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "sivfs_common.h"

//Round down to a multiple of PAGE_SIZE
HEADER_INLINE size_t PAGE_ROUND_DOWN(size_t a){
        return (a / PAGE_SIZE) * PAGE_SIZE;
}

//Input should be prevalidated to avoid overflow
HEADER_INLINE size_t PAGE_ROUND_UP(size_t a){
        return PAGE_ROUND_DOWN(a + PAGE_SIZE - 1);
}

//PxD_ent are the values in checkpoints, should also
//conveniently map to page table entries
typedef phys_addr_t PxD_ent;
HEADER_INLINE phys_addr_t sivfs_pxd_to_pa (PxD_ent entry){
        return entry & PTE_PFN_MASK;
}

HEADER_INLINE bool sivfs_pxd_present (PxD_ent entry){
        //Doing it this way is no slower than checking flags
        //and has the benefit that it also works if entry is just a raw
        //physical address
        return sivfs_pxd_to_pa(entry) != 0;
}

//Sets correct bits on a physical address so it can be inserted into a page
//directory / higher table
HEADER_INLINE PxD_ent sivfs_mk_pxd_ent(phys_addr_t pa){
        return pa | _PAGE_TABLE;
}

//Sets correct bits on a physical address so it can be inserted into a page
//table read only
HEADER_INLINE PxD_ent sivfs_mk_pte(phys_addr_t pa){
        return pa | pgprot_val(PAGE_READONLY_EXEC);
}


//foffset automatically rounded down to PAGE_SIZE boundary
//Unmaps the range in the mapping corresponding to a range of file offsets foffset of size size
HEADER_INLINE void sivfs_unmap_page_range(
        struct address_space *mapping,
        size_t foffset,
        size_t size
){
        //Do something sensible if size = 0.
        WARN_ON_ONCE(size == 0);
        if (size == 0){
                return;
        }

        //Can't fail
        //Unmap wants a start, size pair. Even remove COWs.
        unmap_mapping_range(
                mapping,
                foffset,
                size,
                1
        );

        //Wants an inclusive range
        //Also we want to kick out whole pages.
        truncate_inode_pages_range(
                mapping,
                PAGE_ROUND_DOWN(foffset),
                PAGE_ROUND_UP(foffset + size) - 1
        );
}

//Unmaps an entire mapping. Pretty slow. Kicks out workingset pages and
//checkpoint pages.
void sivfs_unmap_mapping(struct address_space *mapping);

//Kick out workingset pages.
void sivfs_unmap_mapping_workingset(struct address_space *mapping);

struct sivfs_shared_state;
//Finds the vma in current->mm containing virtual address range [start, start+size)
//and validates
// (1) that the range is proper
// (2) that there is a single vma containing the entire range
// (3) (concurrency check) that the vma is a sivfs anon vma owned by current
//
//  (3) is optional and only checked if "check_sivfs_current" is set.
//
//Requires the lock on current's mm to be held
int sivfs_find_vma(
        struct vm_area_struct** vma_out,
        struct sivfs_shared_state* sstate,
        unsigned long start,
        size_t size,
        bool check_sivfs_current
);

#endif //ifndef SIVFS_MM_H

