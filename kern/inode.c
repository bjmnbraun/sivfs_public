/**
  *
  * Things implemented in this file:
  *   - Creation of files, directories, symlinks
  *     (Removal of said is done using default handlers)
  *
  *   - "Page writeback" - can this be avoided?
  *
  *   - Inode and directory operations
  *
  *   - Page fault handlers
  *
  */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/miscdevice.h>
#include <linux/atomic.h>
#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

#include "sivfs.h"
#include "sivfs_common.h"
#include "sivfs_state.h"
#include "sivfs_shared_state.h"
#include "sivfs_inode_info.h"
#include "sivfs_file_info.h"
#include "sivfs_commit_update.h"
#include "sivfs_mm.h"
#include "sivfs_nontxnal_ops.h"
#include "sivfs_make_checkpoint.h"

void dout_page_info_current(unsigned long virtual_address){
        struct sivfs_page_info info;
        int rc = sivfs_get_page_info_current(
                &info,
                virtual_address
        );
        if (rc){
                dout("Error in get_page_info");
        } else {
                size_t buf_size = 256;
                char* buf = kmalloc(buf_size, GFP_KERNEL);
                if (buf){
                        int prc = sivfs_page_info_to_string(buf, buf_size, &info);

                        dout("%.*s", (int)buf_size, buf);

                        kfree(buf);
                } else {
                        dout("Error in get_page_info");
                }
        }
}

//Grumble, linux 4.10 changed the name of a field...
HEADER_INLINE unsigned long vmf_address(struct vm_fault* vmf){
#if SIVFS_COMPAT_LINUX >= SIVFS_COMPAT_LINUX_4_10
        return vmf->address;
#else
        return (unsigned long) vmf->virtual_address;
#endif
}

bool sivfs_checkpoint_mapped(struct mm_struct* mm, unsigned long addr){
        pgd_t* pgd = pgd_offset(mm, addr);
        if (pgd_present(*pgd)){
                pud_t *pud = pud_offset(pgd, addr);
                struct page* pud_page = pfn_to_page(pud_pfn(*pud));
                if (sivfs_is_checkpoint_page(pud_page)){
                        //Then we have a checkpoint mapped in
                        return true;
                }
        }
        return false;
}

void sivfs_unmap_checkpoint_range(
        struct sivfs_state* state,
        struct mm_struct* mm,
        unsigned long start,
        unsigned long end
){
        void** slot;

        if (end > (unsigned long)(0 - PGDIR_SIZE)){
                dout("WARN: End was too large! Reducing it.");
                end = 0 - PAGE_SIZE;
        }

        bool needs_flush = false;

        unsigned long addr;
        for(addr = start; addr < end; addr += PGDIR_SIZE){
                pgd_t* pgd = pgd_offset(mm, addr);
                if (pgd_present(*pgd)){
                        pud_t *pud = pud_offset(pgd, addr);
                        struct page* pud_page = pfn_to_page(pud_pfn(*pud));
                        if (sivfs_is_checkpoint_page(pud_page)){
                                //Then we have a checkpoint mapped in
                                dout("Kicking out checkpoint @ %016lx", addr);
                                pgd_clear(pgd);
                                needs_flush = true;
                        }
                }
        }

        if (needs_flush){
                //Not exported!
                //flush_tlb();
                __native_flush_tlb();
        }

}

//end currently ignored FIXME
void sivfs_free_pgd_range(struct mm_struct* mm, unsigned long start, unsigned long end){
        pgd_t* pgd = pgd_offset(mm, start);
        if (!pgd_present(*pgd)){
                return;
        }

        //XXX actually do the whole range maybe?
        //Currently this works because we are just fixing up the one or two
        //pages Linux (unnecessarily) allocates for us if we decide to check
        //out a checkpoint
        //Not sure what the point of &'ing with PGD_MASK here is, I think it's
        //equivalent to just pass 0 as the second parameter...
        pud_t* pud_pagestart = pud_offset(pgd, start & PGDIR_MASK);

        pud_t* pud = pud_offset(pgd, start);
        if (pud_present(*pud)){
                //We have a pud, so keep going:
                //Again I feel like 0 should be OK for the second parameter
                pmd_t* pmd_pagestart = pmd_offset(pud, start & PUD_MASK);

                pmd_t* pmd = pmd_offset(pud, start);
                if (pmd_present(*pmd)){
                        //We have a pmd, so keep going. Each pmd points to a
                        //pagetable, which is one page of pointers-to-pages.
                        //We are only freeing the page tables, so:
                        pgtable_t pte_pagestart = pmd_pgtable(*pmd);
                        //I don't know how this could be zero but let's be
                        //careful
                        if (pte_pagestart){
                                pmd_clear(pmd);
                                pte_free(mm, pte_pagestart);
                                atomic_long_dec(&mm->nr_ptes);
                        } else {
                                dout("Weird, a present pmd without a pgtable?");
                        }
                }

                pud_clear(pud);
                pmd_free(mm, pmd_pagestart);
                mm_dec_nr_pmds(mm);
        }

        //puds don't seem to have a counter? odd.
        pud_free(mm, pud_pagestart);
}

//Can fail with error code 0 by setting success to false.
int sivfs_try_checkout_checkpoint(
        struct vm_area_struct* vma,
        struct vm_fault* vmf,
        bool* _checked_out_checkpoint,
        struct sivfs_state* state,
        unsigned long file_ino
){
        int ret = 0;
        bool checked_out_checkpoint = false;

        if (SIVFS_FIGURE_CHECKOUT_CPS == SIVFS_FIGURE_NO_CHECKOUT_CPS){
                goto out;
        }

        if (state->direct_access){
                //Not allowed to directly checkout checkpoints in this mode
                goto out;
        }

        //We use cp_page_to_cow as a signal that we faulted due to mkwrite
        //in that case, don't check out a checkpoint again
        if (state->cp_page_to_cow){
                goto out;
        }

        sivfs_ts ts = state->ts;
        if (state->ts == SIVFS_INVALID_TS){
                dout("Page fault outside of a txn");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct inode* checkpoint = state->checkpoint;
        if (!checkpoint){
                dout("No checkpoint checked out - assertion error");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct inode* anon_inode = vma->vm_file->f_inode;
        if (!anon_inode){
                dout("Assertion error");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }
        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(anon_inode);
        if (!iinfo){
                dout("Assertion error");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        //We only increment nrpages inside sivfs_fault, and there we do a
        //concurrency check. So, if we read 0 here it will remain 0 for the
        //remainder of this call.
        if (anon_inode->i_mapping->nrpages > 0 ||
                !sivfs_radix_tree_empty(&iinfo->checkpoint_pages)){
                //There are mapped in pages => Can't check out CP
                goto out;
        }

        int flags = vmf->flags;

        //check alignment here
        //PGDIR mask contains only bits that decide PGDIR index.
        if (vma->vm_start > (vmf_address(vmf) & PGDIR_MASK)
|| vma->vm_end < ((unsigned long)(vmf_address(vmf) + PGDIR_SIZE) & PGDIR_MASK)){
                if (SIVFS_DEBUG_PGFAULTS){
                        dout(
                                "VMA does not contain the PGD containing %016lx"
                                " vma=[%016lx,%016lx), cannot checkout checkpoint. (%016lx)",
                                (unsigned long) vmf_address(vmf),
                                vma->vm_start,
                                vma->vm_end,
                                PGDIR_MASK
                        );
                }
                goto out;
        }

        size_t file_offset =
                ((size_t)(vmf->pgoff) << PAGE_SHIFT)
        ;

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;
        sivfs_ts cp_ts = cpinfo->cp_ts;
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;
        sivfs_ts first_modifier;
        int rc = sivfs_find_first_write(
                sstate,
                &first_modifier,
                cp_ts,
                ts,
                file_ino,
                file_offset & PGDIR_MASK,
                (file_offset + PGDIR_SIZE)&PGDIR_MASK
        );
        if (rc){
                dout("Here");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }
        bool has_concurrent_modifications =
                (first_modifier != SIVFS_INVALID_TS)
        ;
        if (has_concurrent_modifications){
                goto out;
        }

        struct page* pud_pg;

        rc = sivfs_lookup_cp_page_at_level(
                &pud_pg,
                checkpoint,
                file_ino,
                //0 file offset
                0,
                //file_offset,
                //Checkpoint pages are leveled trees
                //Level 0 = leaf page
                //Level 1 = page table (entries are pte_t ~ pointer-to-leaf-page)
                //Level 2 = pmd (entries are pmd_t ~ pointer-to-page-table)
                //Level 3 = pud (entries are pud_t ~ pointer-to-pmd)
                //Level 4 = pgd (entries are pgd_t ~ pointer-to-pud)
                //Level 5 and up have no special name and their entries are
                //simply a pointer to a lower-leveled tree.
                3
        );

        if (rc){
                dout("Here");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        if (!pud_pg){
                //This can occur if we have never written to this file before
                //(or this block of this file)
                goto out;
        }

        pgd_t* pgd = pgd_offset(vma->vm_mm, vmf_address(vmf));

        //Check what's already there
        if (pgd_present(*pgd)){
                pud_t *pud = pud_offset(pgd, vmf_address(vmf));
                struct page* pud_page = pfn_to_page(pud_pfn(*pud));
                if (sivfs_is_checkpoint_page(pud_page)){
                        //Then we have a checkpoint mapped in
                        dout("Here!");
                        ret = VM_FAULT_SIGBUS;
                        goto error0;
                }
        }

        //Clear what's already there
        sivfs_free_pgd_range(
                vma->vm_mm,
                vmf_address(vmf),
                vmf_address(vmf)+1
        );

        dout("Setting %p (currently %lx) to %p %llx @ %p", pgd, pgd->pgd, pud_pg, page_to_pa(pud_pg), checkpoint);

        //pgd_populate takes the result of page_address on a page
        pud_t* pud = page_address(pud_pg);
        pgd_populate(vma->vm_mm, pgd, pud);
        __native_flush_tlb();

        iinfo->checkpoint_checked_out = checked_out_checkpoint = true;
        state->stats.nCheckedOutCheckpoints++;

        dout_page_info_current(
                vmf_address(vmf)
        );

        //For retry returns, need to up the mmap_sem
        ret = VM_FAULT_RETRY;
        up_read(&vma->vm_mm->mmap_sem);

out:
        *_checked_out_checkpoint = checked_out_checkpoint;

error0:
        return ret;
}

/*
 * Fast path for sivfs_fault, tries to check out a page directly from a
 * checkpoint if it is safe to do so (no modifications up to ts and we don't
 * have a workingset page).
 *
 * Page is "got" with one refct and also locked
 */
int sivfs_fault_checkpoint(
        struct vm_area_struct* vma,
        struct vm_fault* vmf,
        struct page** pg_out,
        struct sivfs_state* state,
        unsigned long ino
){
        int ret = 0;
        int rc = 0;
        struct page* cp_page = NULL;

        if (SIVFS_FIGURE_CHECKOUT_CP_PAGES == SIVFS_FIGURE_NO_CHECKOUT_CP_PAGES){
                goto out;
        }

        //Translate fault address into file offset
        //This requires current->mm semaphore to be held, but AFAIK this is
        //guaranteed whenever in page fault.
        //
        //Currently doesn't support vm_start not page aligned
        //since this could lead to non-page-aligned faults which get messy

        //Mmaps are always offset into the file page aligned.
        size_t file_offset =
                ((size_t)(vmf->pgoff) << PAGE_SHIFT)
        ;

        sivfs_ts ts = state->ts;
        if (state->ts == SIVFS_INVALID_TS){
                dout("Page fault outside of a txn");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct inode* checkpoint = state->checkpoint;
        if (!checkpoint){
                dout("No checkpoint checked out - assertion error");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;

#if 1
        sivfs_ts cp_ts = cpinfo->cp_ts;
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;
        sivfs_ts first_modifier;
        rc = sivfs_find_first_write(
                sstate,
                &first_modifier,
                cp_ts,
                ts,
                ino,
                file_offset, file_offset + PAGE_SIZE
        );
        if (rc){
                dout("Here");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }
        bool has_concurrent_modifications =
                (first_modifier != SIVFS_INVALID_TS)
        ;
#elif 0
        bool has_concurrent_modifications = (cpinfo->cp_ts != ts);
#else
        //BAD!
        bool has_concurrent_modifications = false;
#endif
        if (has_concurrent_modifications){
                goto out;
        }

        //We can't have a workingset page checked out:
        struct address_space* mapping = vma->vm_file->f_mapping;

        struct page* mapped_page = radix_tree_lookup(&mapping->page_tree, vmf->pgoff);
        if (mapped_page){
                goto out;
        }

        //Return codes of this are same as sivfs_fault
        ret = sivfs_lock_cp_page(
                &cp_page,
                checkpoint,
                ino,
                file_offset,
                vmf->flags
        );

        if ((ret & VM_FAULT_ERROR) || (ret & VM_FAULT_RETRY)){
                //Pass codes up
                goto error0;
        }

        //Weird sanity check
        if (cp_page && !sivfs_is_checkpoint_page(cp_page)){
                dout("Here");
                ret = VM_FAULT_SIGBUS;
                goto error1;
        }

        if (state->direct_access){
                bool is_zero_page =
                        cp_page && sivfs_is_zero_page(cp_page)
                ;

                //Absence of page or zero page, we patch up:
                if (!cp_page || is_zero_page){
                        if (cp_page){
                                //Unlock the page before the patch
                                unlock_page(cp_page);
                                put_page(cp_page);
                                cp_page = NULL;
                        }

                        rc = sivfs_prepare_direct_access_page(
                                state,
                                checkpoint,
                                ino,
                                file_offset
                        );
                        if (rc){
                                dout("Here");
                                ret = VM_FAULT_SIGBUS;
                                goto error1;
                        }

                        if (SIVFS_DEBUG_DIRECT_ACCESS){
                                dout("Patched up checkpoint for direct access %lx", file_offset);
                        }

                        //Retry
                        up_read(&vma->vm_mm->mmap_sem);
                        ret = VM_FAULT_RETRY;
                        goto error1;
                }

        }


        if (!cp_page){
                //No checkpoint page. return up.
                goto out;
        }

#if 0
        //ZERO_PAGE(0) is not PageUptodate so this sanity check doesn't work
        if (unlikely(!PageUptodate(cp_page))){
                dout("Here %016llx", page_to_pa(cp_page));
                ret = VM_FAULT_SIGBUS;
                goto error1;
        }
#endif

        struct sivfs_inode_info* iinfo = sivfs_vma_to_iinfo(vma);
        if (!iinfo){
                dout("Here");
                ret = VM_FAULT_SIGBUS;
                goto error1;
        }

        //It might already exist, in which case we don't care. Can't just
        //insert (void*)true because that value might be reserved for the tag
        //bits. Instead, EXCEPTIONAL_ENTRY is guaranteed to be a "safe" value
        //for radix trees.
        void* entry = (void*)(RADIX_TREE_EXCEPTIONAL_ENTRY);
        rc = sivfs_radix_tree_set(&iinfo->checkpoint_pages, vmf->pgoff, entry);
        if (rc){
                dout("Here %d", rc);
                ret = VM_FAULT_SIGBUS;
                goto error1;
        }


#if 0
        //Checkpoint pages aren't part of the workingset, we don't usually
        //flush them out.

#if SIVFS_TRACK_CLEAR_WORKINGSET_EVERY_TXN
        struct sivfs_workingset_range range = {
                .start = file_offset,
                .size = PAGE_SIZE
        };
        rc = sivfs_workingset_ranges_add(
                &iinfo->anon_ranges,
                range
        );
        if (rc) {
                dout("Here");
                ret = VM_FAULT_SIGBUS;
                goto error1;
        }
#endif
#endif

out:
        vmf->page = cp_page;
        *pg_out = cp_page;
        return ret;

error1:
        if (cp_page){
                unlock_page(cp_page);
                put_page(cp_page);
        }

error0:
        return ret;
}

HEADER_INLINE int sivfs_check_state_current(struct sivfs_state* state){
        //Concurrency check
        struct sivfs_state* state_check = sivfs_task_to_state(current);
        if (!state_check){
                dout("Here");
                return -EINVAL;
        }
        if (state != state_check){
                dout("Here");
                return -EINVAL;
        }

        return 0;
}

/**
 * sivfs_fault - read in file data for page fault handling
 * @vma:        vma in which the fault was taken
 * @vmf:        struct vm_fault containing details of the fault
 *
 * filemap_fault() is invoked via the vma operations vector for a
 * mapped memory region to read in file data during a page fault.
 *
 * vma->vm_mm->mmap_sem is held on entry.
 *
 * If our return value has VM_FAULT_RETRY set, it's because
 * lock_page_or_retry() returned 0.
 * The mmap_sem has usually been released in this case.
 * See __lock_page_or_retry() for the exception.
 *
 * If our return value does not have VM_FAULT_RETRY set, the mmap_sem
 * has not been released.
 *
 * We never return with VM_FAULT_RETRY and a bit from VM_FAULT_ERROR set.
 */
int sivfs_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
        int ret = 0;
        int rc = 0;

        if (SIVFS_DEBUG_PGFAULTS) dout("Handling page fault");

        //Lookup sivfs info from vma
        struct sivfs_inode_info* iinfo = sivfs_vma_to_iinfo(vma);

        if (!iinfo){
                dout("Assertion error.");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct sivfs_state* state = iinfo->state;

        if (!state){
                dout("Huh? no state in anon iinfo!");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        rc = sivfs_check_state_current(state);
        if (rc){
                dout("Faulting thread couldn't access state in vma");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        if (SIVFS_DEBUG_PGFAULTS) dout(
                "Pgfault: %lu %llu",
                atomic64_read(&state->checkpoint_ts),
                state->ts
        );

        unsigned long file_ino = iinfo->backing_inode->i_ino;

        //Mmaps are always offset into the file page aligned.
        size_t file_offset =
                ((size_t)(vmf->pgoff) << PAGE_SHIFT)
        ;

        //Process:
        // 1) Check if we have a checkpoint checked out. If we do, we can check
        // it out into this mmap. If we have the checkpoint already checked out
        // into this MMAP, that's an error.
        //
        // --- Note: if you fail to check out the checkpoint, need to kick it
        // out in other mmaps before proceeding. Not doing so creates a problem
        // if the workingset page is then promoted.
        //
        // 2) Check if we have a checkpoint page checked out. If so, we can
        // check it out again unless cp_page_to_cow is set.
        //
        // --- Note: If you fail to check out the checkpoint page, need to kick
        // it out. Same reason as above.
        //
        // 3) Otherwise, create or get the workingset page. If we have a
        // checkpoint page to copy on write, then there must not be a
        // workingset page and we can do a copy rather than geting the page up
        // to date.
        //


        bool checked_out_checkpoint = false;
        ret = sivfs_try_checkout_checkpoint(
                vma,
                vmf,
                &checked_out_checkpoint,
                state,
                file_ino
        );
        if ((ret & VM_FAULT_ERROR) || (ret & VM_FAULT_RETRY)){
                goto error0;
        }

        bool initial_write = vmf->flags & VM_FAULT_WRITE;
        if (initial_write){
                state->stats.nCoWCPPagesInitialWrite++;
        }

        if (checked_out_checkpoint && !initial_write){
                //Done.
        } else if (iinfo->checkpoint_checked_out){
                dout("Not yet implemented - need to kick checkpoint out of other mmaps");
                ret = VM_FAULT_SIGBUS;
                goto error_free_fault_pg;
        }

        //Try to check out just a checkpoint page
        struct page* cp_page = NULL;
        ret = sivfs_fault_checkpoint(
                vma,
                vmf,
                &cp_page,
                state,
                file_ino
        );
        if ((ret & VM_FAULT_ERROR) || (ret & VM_FAULT_RETRY)){
                goto error0;
        }

        //Special handling if we are CoW'ing a checkpoint page.
        struct page* cow_cp_page = state->cp_page_to_cow;
        state->cp_page_to_cow = NULL;

        bool should_cow_cp_page = cow_cp_page == cp_page;
        if (should_cow_cp_page){
                state->stats.nCoWCPPagesMkwrite++;
        }

        if (state->direct_access){
                bool is_zero_page =
                        cp_page && sivfs_is_zero_page(cp_page)
                ;

                if (!cp_page || should_cow_cp_page || is_zero_page){
                        dout(
                                "Couldn't check out checkpoint page in direct access!"
                                " @ %lu %lu %p %p %s", 
                                file_ino,
                                file_offset,
                                cp_page, 
                                cow_cp_page,
                                is_zero_page?"ZERO_PAGE":"0"
                        );
                        ret = VM_FAULT_SIGBUS;
                        //Just drop the cow_cp_page on the floor.
                        goto error0;
                }
                //We have a checkpoint page and we can return it.
                state->stats.nCheckoutCPPages++;
                //Return the cp_page
                goto out;
        }


        if (cp_page && !initial_write && !should_cow_cp_page){
                //We have a checkpoint page and we can return it.
                state->stats.nCheckoutCPPages++;
                //Return the cp_page
                goto out;
        }

        if (cp_page){
                //If we get here, we did not return the cp_page. So unlock it.
                if (ret & VM_FAULT_LOCKED){
                        unlock_page(cp_page);
                }
                put_page(cp_page);

                ret = 0;
        }

        //We failed to check out a checkpoint page, so evict any.
        struct address_space* mapping = vma->vm_file->f_mapping;
        {
                //Evict any checkpoint entry here
                void* checkpoint_entry = radix_tree_lookup(
                        &iinfo->checkpoint_pages,
                        vmf->pgoff
                );
                if (unlikely(checkpoint_entry && checkpoint_entry != SIVFS_RADIX_TREE_TOMBSTONE)){
                        //Evict the page. Note that current->mm is held on
                        //entry.
                        sivfs_unmap_page_range(
                                mapping,
                                PAGE_ROUND_DOWN(file_offset),
                                PAGE_SIZE
                        );
                        //sivfs_radix_tree_tombstone(&iinfo->checkpoint_pages, vmf->pgoff);
                        radix_tree_delete(&iinfo->checkpoint_pages, vmf->pgoff);
                }
        }

#if 0
        //filemap_fault allocates a page, and then tries to fill it by calling
        //readpage. Instead, we know we want a page of zeros so... Also we can
        //gain to avoid a few locks by tailoring what we need, see below.

        //This won't work, because
        filemap_fault is returning pages as UpToDate on the first fault!

        ret = filemap_fault(vma, vmf);
#else
        {
                //Page comes with a refct
                struct page* page = find_get_page(mapping, vmf->pgoff);
                if (!page){
                        page = page_cache_alloc(mapping);
                        if (!page){
                                dout("Here");
                                ret = VM_FAULT_SIGBUS;
                                goto error1;
                        }

                        int rc = 0;
                        //rc = add_to_page_cache(
                        rc = add_to_page_cache_lru(
                                page,
                                mapping,
                                vmf->pgoff,
                                mapping_gfp_constraint(mapping, GFP_KERNEL)
                        );

                        //Concurrent additions should never happen
                        if (rc){
                                dout("Here");
                                put_page(page);
                                ret = VM_FAULT_SIGBUS;
                                goto error1;
                        }

                        ret = VM_FAULT_LOCKED;
                } else {
                        if (!trylock_page(page)){
                                dout("Here");
                                put_page(page);
                                up_read(&vma->vm_mm->mmap_sem);
                                ret = VM_FAULT_RETRY;
                                goto error1;
                        }

                        if (unlikely(page->mapping != mapping)){
                                dout("Here");
                                unlock_page(page);
                                put_page(page);
                                up_read(&vma->vm_mm->mmap_sem);
                                ret = VM_FAULT_RETRY;
                                goto error1;
                        }

                        if (unlikely(!PageUptodate(page))){
                                //This is apparently an error
                                dout("Here");
                                unlock_page(page);
                                put_page(page);
                                ret = VM_FAULT_SIGBUS;
                                goto error1;
                        }

                        ret = VM_FAULT_LOCKED;
                }

                vmf->page = page;
        }
#endif

        int fault_ret = ret;

        //Pass errors or retries up
        if ((ret & VM_FAULT_ERROR) || (ret & VM_FAULT_RETRY)){
                goto error1;
        }

        //if we didn't return a page segfault
        if (!vmf->page){
                dout("HUH? No page!");
                ret = VM_FAULT_SIGBUS;
                goto error1;
        }

        //Faulted page usually has a refct of >= 2.
        int refct = page_ref_count(vmf->page);
        WARN_ON_ONCE(refct == 0);

        //We use PageUptodate to indicate whether the filemap_fault or such
        //call above mapped a fresh page (which is not uptodate) or a page
        //already in the workingset. The latter occurs if the page fault was
        //spurious (might never happen) or just if the file is mmap'ed twice
        //into the processes address space.
        //
        //Workingset pages become Uptodate once on fault in, and then stay
        //Uptodate even if modified by the user until evicted from the
        //workingset. If we page out a workingset page, we need to load it back
        //in as Uptodate - so swapping out workingset pages needs to be a bit
        //careful.
        if (PageUptodate(vmf->page)){
                goto page_uptodate;
        }

        //Otherwise fresh workingset page, fill the page with the snapshot
        //contents
        void* mem = kmap(vmf->page);

        if (cp_page){
                //We are CoW'ing a checkpoint page
                void* cp_mem = kmap(cp_page);
                memcpy(mem, cp_mem, PAGE_SIZE);
                kunmap(cp_page);
        } else {
                //Need to look up a checkpoint page, copy it, and apply logs
                rc = sivfs_update_page(
                        mem,
                        state,
                        vma,
                        file_offset
                );
        }
        kunmap(vmf->page);
        if (rc){
                ret = VM_FAULT_SIGBUS;
                goto error_free_fault_pg;
        }

        __SetPageUptodate(vmf->page);

page_uptodate:
        //So, apparently, kswapd refuses to reclaim a dirty, shared, mapped
        //page unless this is set. For proof, see vmscan.c last if statement in
        //page_check_references.
        __SetPageSwapBacked(vmf->page);

#if SIVFS_TRACK_CLEAR_WORKINGSET_EVERY_TXN
        //For checkpoint page CoW's, we add the range again
        //but that's OK
        struct sivfs_workingset_range range = {
                .start = file_offset,
                .size = PAGE_SIZE
        };
        rc = sivfs_workingset_ranges_add(
                &iinfo->anon_ranges,
                range
        );
        if (rc) {
                dout("Here");
                ret = VM_FAULT_SIGBUS;
                goto error_free_fault_pg;
        }
#endif

        //Check-out-a-checkpoint jumps here
out:
        //Good out
        if (SIVFS_DEBUG_PGFAULTS) dout("Page fault ret %x %p", ret, vmf->page);

        state->stats.nPgFaults++;

        return ret;

error_free_fault_pg:
        if (vmf->page){
                if (fault_ret & VM_FAULT_LOCKED){
                        unlock_page(vmf->page);
                }
                put_page(vmf->page);
        }
        vmf->page = NULL;

error1:
        //No cleanup needed for cp_page.

error0:
        return ret;
}

//Assumes !is_vm_hugetlb_page(vma)
static inline pgoff_t _linear_page_index(struct vm_area_struct *vma, unsigned long address)
{
        pgoff_t pgoff;
        pgoff = (address - vma->vm_start) >> PAGE_SHIFT;
        pgoff += vma->vm_pgoff;
        return pgoff;
}

int sivfs_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf){
        //Already have a page, just make it writable, or fail with NOPAGE if we
        //need to force a CoW.
        //
        //Due to a bug (?) or undefined behavior, vmf->pgoff is based on the
        //->index field of the page currently mapped in. This means that all
        //zero pages lead to a wrong pgoff.
        //
        //vmf->virtual_address, however, is correctly computed in all cases.

        int ret = 0;

        //Lookup sivfs info from vma
        struct sivfs_inode_info* iinfo = sivfs_vma_to_iinfo(vma);

        if (!iinfo){
                dout("Assertion error.");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct sivfs_state* state = iinfo->state;
        if (!state){
                dout("Huh? no state in anon iinfo!");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        int rc = sivfs_check_state_current(state);
        if (rc){
                dout("Faulting thread couldn't access state in vma");
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        if (state->cp_page_to_cow){
                dout("Never CoW'd page: %016llx", page_to_pa(state->cp_page_to_cow));
                dout("Is zero page: %d", sivfs_is_zero_page(state->cp_page_to_cow));
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        if (state->direct_access){
                struct page *page = vmf->page;
                struct inode *inode = file_inode(vma->vm_file);

                if (!sivfs_is_checkpoint_page(page)){
                        dout("Direct access to non-cp page, error!");
                        ret = VM_FAULT_SIGBUS;
                        goto error0;
                }

                sb_start_pagefault(inode->i_sb);
                file_update_time(vma->vm_file);
                lock_page(page);
                /*
                 * We mark the page dirty already here so that when freeze is in
                 * progress, we are guaranteed that writeback during freezing will
                 * see the dirty page and writeprotect it again.
                 */
                set_page_dirty(page);

                //wait_for_stable_page(page) segfaults, we don't need it anyway.
                //this is because we don't set up "mapping" properly for
                //checkpoint pages

                ret = VM_FAULT_LOCKED;

                sb_end_pagefault(inode->i_sb);

                //Easy - just write to the page.
                goto out;
        }

        struct page* page = vmf->page;
        size_t pgoff = _linear_page_index(
                vma,
                vmf_address(vmf)
        );
        size_t file_offset = pgoff << PAGE_SHIFT;
        struct address_space* mapping = vma->vm_file->f_mapping;
        if (sivfs_checkpoint_mapped(vma->vm_mm, vmf_address(vmf))){
                //We have to unmap the checkpoint - a write was attempted
                //sivfs_unmap_checkpoint(vma);
                sivfs_unmap_page_range(
                        mapping,
                        PAGE_ROUND_DOWN(file_offset),
                        PAGE_SIZE
                );
                if (!iinfo->checkpoint_checked_out){
                        dout("Assertion error.");
                        ret = VM_FAULT_SIGBUS;
                        goto error0;
                }
                iinfo->checkpoint_checked_out = false;

                //On the refault, we don't want to checkout another checkpoint.
                //We signal this by setting cp_page_to_cow, which is safe
                state->cp_page_to_cow = page;
                ret = VM_FAULT_NOPAGE;

                dout("Refaulting after unmapping checked out checkpoint");
        } else if (sivfs_is_checkpoint_page(page)){
                //Unmap the page and keep some metadata so that on retry
                //of the fault we copy the checkpoint page.

                //Computing file offset of the fault is tricky, since we don't
                //have vmf->pgoff. Instead, gather it from virtual address
                if (unlikely(is_vm_hugetlb_page(vma))) {
                        dout("Assertion error");
                        ret = VM_FAULT_SIGBUS;
                        goto error0;
                }

                state->cp_page_to_cow = page;
                if (SIVFS_DEBUG_PGFAULTS) {
                        dout(
                                "Want to CoW page: %016llx %d",
                                page_to_pa(page),
                                page_ref_count(page)
                        );
                }

                sivfs_unmap_page_range(
                        mapping,
                        file_offset,
                        PAGE_SIZE
                );
                radix_tree_delete(&iinfo->checkpoint_pages, pgoff);

                if (SIVFS_DEBUG_PGFAULTS) {
                        dout_page_info_current(vmf_address(vmf));
                        dout("Refaulting to CoW page %016llx %d",
                        page_to_pa(page), page_ref_count(page));
                }

                ret = VM_FAULT_NOPAGE;
        } else {
                ret = filemap_page_mkwrite(vma, vmf);
        }

out:
error0:
        return ret;

}

static int
sivfs_write_begin(struct file *file, struct address_space *mapping,
                        loff_t pos, unsigned len, unsigned flags,
                        struct page **pagep, void **fsdata)
{
        dout("Here!");
        return -ENOMEM;
}

static int
sivfs_write_end(struct file *file, struct address_space *mapping,
                        loff_t pos, unsigned len, unsigned flags,
                        struct page **pagep, void **fsdata)
{
        return -ENOMEM;
}

//Called under memory pressure / whenever Linux wants to reclaim a dirty page
//clean pages can (always?) be reclaimed.
//
//page is locked, and the caller has already marked the page non-dirty (oddly
//enough)
int sivfs_writepage(struct page* page, struct writeback_control *wbc){
        int rc = 0;
        if (PageDirty(page)){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }
        if (sivfs_is_checkpoint_page(page)){
                dout("Don't swap out checkpoint pages (yet)");
                rc = -EINVAL;
                goto error0;
        }
        if (PagePrivate(page)){
                dout("Huh... can't deal with private on anon pages");
                rc = -EINVAL;
                goto error0;
        }
        if (!PageUptodate(page)){
                dout("Weird... workingset pages should always be Uptodate");
                rc = -EINVAL;
                goto error0;
        }

        if (!PageReclaim(page)){
                dout("Weird... I think we only get here from page reclaim!");
                rc = -EINVAL;
                goto error0;
        }

        if (PageWriteback(page)){
                dout("Weird... writepage called twice on page?");
                rc = -EINVAL;
                goto error0;
        }

        if (!wbc->for_reclaim){
                //We get here if the user explicitly trys to use fsync or
                //something fancy. Error out.
                dout("Fsync and friends not yet supported");
                rc = -EINVAL;
                goto error0;
        }

        //TODO If we get here, need to add the page to swap, i.e. talk to the
        //swap device. For now, just pretend the swap couldn't take any more
        //pages:
        goto redirty;

        //The below just throws the page away - this is incorrect!
#if 0
        set_page_writeback(page);
        unlock_page(page);
        end_page_writeback(page);
#endif

error0:
        return rc;

redirty:
        set_page_dirty(page);
        return AOP_WRITEPAGE_ACTIVATE; /* return with page locked */
}

int sivfs_set_page_dirty(struct page* page){
        int ret = 0;

        if (sivfs_is_checkpoint_page(page)){
                struct sivfs_state* state = sivfs_task_to_state(current);
                if (!state) {
                        WARN_ON_ONCE(true);
                } else {
                        if (state->direct_access){
                                //OK!
                        } else {
                                //We should prevent this by page_mkwrite, but if we get here
                                //this is a bug. We warn and just return.
                                WARN_ON_ONCE(true);
                        }
                }
        }
#if 0
        //Hmm, do the real thing to get radix_tree_tag'ed support but of course
        //this is slower. Reevaluate whether this is a win over just always
        //iterating over the whole tree and checking on each page whether it is
        //dirty.
        //
        //This also marks the inode as having dirty pages, necessary
        //for calling writepages on close
        ret = __set_page_dirty_nobuffers(page);
        //ret = __set_page_dirty_buffers(page);
#else
        //We do want to writeback to swap, so don't do this:

        //Copied from __set_page_dirty_no_writeback
        if (!PageDirty(page))
                return !TestSetPageDirty(page);
#endif

error0:
        return ret;
}

#if 0
void sivfs_vma_close(struct vm_area_struct *vma) {
        if (SIVFS_DEBUG_MMAPS) dout("In vma close %p", vma);
}
#endif

//Checks whether a vma belongs to us
bool sivfs_is_vma_owned(
        struct vm_area_struct* vma,
        struct sivfs_shared_state* sstate
){
        if (!vma->vm_file){
                return false;
        }

        if (!vma->vm_file->f_inode){
                dout("Weird - I didn't think this could happen");
                return false;
        }

        if (vma->vm_file->f_inode->i_sb != sstate->sb){
                return false;
        }

        return true;
}


#if 0
static void sivfs_change_pte(
        struct mmu_notifier *mn,
        struct mm_struct *mm,
        unsigned long address,
        pte_t pte
)
{
        dout("MM change pte!");
}

static void sivfs_invalidate_page(
        struct mmu_notifier *mn,
        struct mm_struct*mm,
        unsigned long address
)
{
        dout("MM invalidate page!");
}
#endif

static void sivfs_invalidate_range_start(
        struct mmu_notifier *mn,
        struct mm_struct *mm,
        unsigned long start,
        unsigned long end
)
{
        if(SIVFS_DEBUG_MMU_NOTIFIER){
                //This is a bit verbose
                dout("MM invalidate range! %016lx %016lx", start, end);
        }

        struct sivfs_state *state = container_of(
                mn,
                struct sivfs_state,
                notifier
        );
        if (!state->notifier_registered){
                dout("Error: invalidate_range_start() called when notifier was not registered!");
        }

        sivfs_unmap_checkpoint_range(state, mm, start, end);

out:
        ;
}

static void sivfs_mm_release(struct mmu_notifier *mn, struct mm_struct *mm)
{
        if(SIVFS_DEBUG_THREADS){
                dout("MM released!");
        }

        struct sivfs_state *state = container_of(
                mn,
                struct sivfs_state,
                notifier
        );
        if (!state->notifier_registered){
                dout("Error: release() called when notifier was not registered!");
        }

        sivfs_invalidate_range_start(
                mn,
                mm,
                0,
                (unsigned long)(0 - PGDIR_SIZE)
        );

        //Use this as an opportunity to clear out anon inodes that are
        //UNREFERENCED, the idea being that mm_release is definitely only
        //called at thread death

        struct sivfs_workingset* ws = &state->workingset;
        void** slot;
        struct radix_tree_iter iter;
        const size_t DELETE_BATCH = 16;
        unsigned long indices[DELETE_BATCH];
        int nr,i;
        size_t index = 0;

        //remove all entries in anon_dentries - automatically removes
        //elements from anon_inodes.
        //
        //We can't just leave these to be cleaned up automatically on unmount,
        //because they are "pseudo" dentries that aren't in the directory tree
        //
        //TODO decouple adding to the tree and creation, so we don't have to
        //have the hacky check below that prevents infinite loop
        do {
                nr = 0;
                radix_tree_for_each_slot(
                        slot,
                        &ws->anon_dentries,
                        &iter,
                        index
                ){
                        struct dentry* anon_dentry = *slot;
                        if (!anon_dentry){
                                dout("Assertion error");
                                return;
                        }
                        struct inode* anon_inode = anon_dentry->d_inode;
                        if (!anon_inode){
                                dout("Assertion error");
                                return;
                        }
                        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(anon_inode);
                        if (!iinfo){
                                dout("Assertion error");
                                return;
                        }
                        if (iinfo->refcount != SIVFS_UNREFERENCED){
                                continue;
                        }

                        dout("Found unreferenced anon inode at thread death, removing %p", anon_inode);

                        //We found an unreferenced anon inode! remove it.
                        indices[nr] = iter.index;
                        if (++nr == DELETE_BATCH)
                                break;
                }
                if (nr > 0){
                        for(i = 0; i < nr; i++){
                                index = indices[i];
                                sivfs_put_anon_inode(state, index);
                                if (radix_tree_lookup(&ws->anon_dentries, index)){
                                        dout("Assertion error");
                                        return;
                                }
                        }
                }
                //If we go through the loop again, start at index+1
                index++;
        } while (nr > 0);

        //Unset registered here so that if we free state below we don't whine about
        //having a registered mmu notifier
        state->notifier_registered = false;

        //Remove the reference from the mmu notifier
        sivfs_put_task_state(state->t_owner);
}

const struct mmu_notifier_ops sivfs_mmuops = {
        .release = sivfs_mm_release,
        //.change_pte = sivfs_change_pte,
        //.invalidate_page = sivfs_invalidate_page,
        //.invalidate_range = sivfs_invalidate_range,
        .invalidate_range_start = sivfs_invalidate_range_start,
};

//Takes a struct file* of an open anonymous file, and
//open the underlying file for direct access into out_file
int sivfs_open_direct_access(struct file** out_file, struct file* file){
        int rc = 0;

        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        //We use direct_access_creds as a way to
        //open the file in a different way (specifically, we select a different
        //inode in select_inode) but the api for vfs_select_inode doesn't pass
        //in enough information for us to know that we want the underlying
        //file. We could use file_flags for this purpose, but that's even more
        //of a hack and then we have a security problem because a user could
        //potentially use the same file_flags and get direct access.
        //
        //This is actually not much of a hack, we could imagine a mode where
        //the user always has such credibilities, so its opens always directly
        //open the underlying file (not yet supported since supporting this
        //requires implementing some more function pointers)

        const struct cred* saved_cred = override_creds(
                sstate->direct_access_creds
        );

        struct path* path = &file->f_path;

        struct file* real_file = dentry_open(
                        path,
                        //O_LARGEFILE is barf
                        O_RDWR | O_LARGEFILE,
                        //current_cred == direct_access_creds here
                        //but we need to override to get the right file
                        //during select_inode
                        current_cred()
                        //sstate->direct_access_creds
                        );

        //Restore creds
        revert_creds(saved_cred);

        if (IS_ERR(real_file)){
                rc = PTR_ERR(real_file);
                goto error0;
        }

out:
        *out_file = real_file;
        return rc;

error0:
        return rc;
}

int sivfs_anon_open(struct inode* inode, struct file * filp){
        int rc = 0;
        //inode was previously selected via select_inode
        if (SIVFS_DEBUG_INODES) dout("In sivfs_open %p %p", inode, filp);

        struct sivfs_file_info* finfo;
        rc = sivfs_new_finfo(&finfo);
        if (rc)
                goto error0;

        struct file* direct_access_file = NULL;
        rc = sivfs_open_direct_access(&direct_access_file, filp);
        if (rc)
                goto error1;

        struct sivfs_state* state;
        //Get the state associated with current, creating one if needed and
        //otherwise just incrementing the reference counter so it is not freed
        state = sivfs_task_to_state(current);
        if (!state) {
                rc = -EPERM;
                dout("Assertion error");
                goto error2;
        }

        //Fill in finfo
        finfo->file = filp;
        finfo->state = state;
        finfo->direct_access_file = direct_access_file;

        //Fill in filp
        filp->private_data = finfo;

        //Increment the refcount
        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(inode);
        if (!iinfo){
                dout("Assertion error - no iinfo!");
                goto error2;
        }
        if (iinfo->refcount == SIVFS_UNREFERENCED){
                iinfo->refcount = 1;
        } else {
                rc = sivfs_iinfo_refct_inc(
                        iinfo
                );
                if (rc){
                        goto error2;
                }
        }

//Good out
        return rc;

error2:
        fput(direct_access_file);

error1:
        sivfs_put_finfo(finfo);

error0:
        return rc;
};

//
//Called on close() of the file returned by open once all mmaps are closed
//used to reclaim private file / inode information
//
//Is definitely called before the process can exit (so task_struct current is
//still accurate)
//
//In our case, this is also our hook to decrement the reference count on an
//anonymous inode (which always has at least one reference count to prevent it
//from being evicted)
//
//Strangely, this is defined as returning an int - but we always succeed
//
int sivfs_anon_release(struct inode* inode, struct file* file){
        if (SIVFS_DEBUG_INODES) dout("In sivfs anon release %p", file);
        struct sivfs_state* state = sivfs_file_to_state(file);
        //Destroy the file_info
        struct sivfs_file_info* finfo = sivfs_file_to_finfo(file);
        sivfs_put_finfo(finfo);

        //Remove a reference on the anon inode
        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(inode);
        if (!iinfo){
                dout("Assertion error - no iinfo!");
                goto out;
        }
        sivfs_put_anon_inode(state, iinfo->backing_inode->i_ino);

        //fput will dput the dentry after we return once. But, at this point
        //the inode should have a null iinfo and the reference to state is
        //gone.

out:
        return 0;
}

int sivfs_drop_inode(struct inode *inode){
        int drop = generic_drop_inode(inode);
        //sivfs_state may not exist here!
        if (drop){
                if (SIVFS_DEBUG_INODES) dout("Dropping inode %p", inode);
                struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(inode);
                if (iinfo){
                        sivfs_put_iinfo(iinfo);
                }
        }
        return drop;
}

/*
void sivfs_evict_inode(struct inode *inode){
        truncate_inode_pages_final(&inode->i_data);
        size_t after_size = sivfs_radix_tree_size(&inode->i_data.page_tree);
        if (after_size){
                dout("ERROR: Pages left over in page cache?");
        }
        clear_inode(inode);
}
*/

//Copy of generic_file_vm_ops
const struct vm_operations_struct sivfs_anon_file_vm_ops = {
        .fault                  = sivfs_fault,
        //.map_pages              = sivfs_map_pages,
        .page_mkwrite           = sivfs_mkwrite,
        //.close                  = sivfs_vma_close,
};

//Set attributes of anon_inode to match the state of the file
//at the current transaction timestamp (i.e. snapshot time)
static int sivfs_set_attributes_txn_ts(
        struct sivfs_state* state,
        struct inode* anon_inode
) {
        int rc = 0;
        if (!sivfs_in_txn(state)){
                dout("Assertion error");
                rc = -EPERM;
                goto error0;
        }

        //sivfs_ts txn_ts = sivfs_txn_ts(state)

        //TODO pull up the attributes of the file as of txn_ts
        //For now we just use the most current one... this is no good!
        //This code needs to read logs or something.
        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(anon_inode);
        //Properly size the inode
        size_t size = i_size_read(iinfo->backing_inode);
        if (SIVFS_DEBUG_INODES) dout("Old inode size is %zd", size);
        truncate_setsize(anon_inode, size);

out:
        //Good out
        return rc;

error0:
        return rc;
}


int sivfs_anon_mmap(struct file * file, struct vm_area_struct * vma){
        int rc = 0;

        if (!(vma->vm_flags & VM_SHARED)
                && (vma->vm_flags & VM_WRITE)){
                //TODO find out if we can support PRIVATE mappings. For now,
                //just reject them:
                //dout("INFO: VM_PRIVATE mappings incur unnecessary CoWs.");
                dout("VM_PRIVATE mappings on sivfs are not yet supported. Use VM_SHARED");
                rc = -EINVAL;
                goto error0;
        }

        vma->vm_ops = &sivfs_anon_file_vm_ops;

        struct sivfs_state* state = sivfs_file_to_state(file);

        //Finally, we need to load the attributes of the file into the
        //invisible inode at the snapshot time of the txn
        //TODO right now we don't update attributes after the initial mmap
        //to do that, this needs to also go into commit_update.
        rc = sivfs_set_attributes_txn_ts(state, file->f_inode);
        if (rc)
                goto error0;

        //Probably optional, I think this just sets the last accessed time
        file_accessed(file);

        if (SIVFS_DEBUG_MMAPS) dout("Successful mmap");

//Good out
out:
        return rc;

error0:
        return rc;
}

bool sivfs_is_anon_inode(struct inode* inode){
        return inode->i_op == &sivfs_anon_file_inode_operations;
}

bool sivfs_is_anon_dentry(struct dentry* dentry){
        struct inode* inode = d_inode(dentry);
        if (!inode)
                return false;
        return sivfs_is_anon_inode(inode);
}

//Writes into out_inode
int sivfs_create_anon_inode(
        struct inode** out_inode,
        struct inode* file_inode,
        struct sivfs_state* state
) {
        int rc = 0;

        //Nodes from get_inode are in the I_NEW state and, while in the
        //superblock's s_inode list, cannot be reclaimed if the filesystem is
        //mounted.
        //There is a reference on the inode (i.e. it has been igot.) This
        //reference is due to the inode being anonymous, we should manually
        //remove this via iput when removing the anonymous link
        struct inode * inode = sivfs_get_inode(
                file_inode->i_sb,
                NULL,
                0600 | S_IFREG,
                0
        );
        if (!inode){
                rc = -ENOMEM;
                goto error0;
        }

        //We have to set up the mmu notifier on state if we are going to be
        //mmaping in the file:
        //If the mmap fails, we keep the mmu notifier attached.
        //TODO figure out if that's a big deal
        if (!state->notifier_registered){
                struct mm_struct* mm = get_task_mm(state->t_owner);
                if (!mm){
                        dout("Here");
                        rc = -EINVAL;
                        goto error_free;
                }
                rc = sivfs_get_task_state(state->t_owner);
                if (rc){
                        dout("Here");
                        mmput(mm);
                        goto error_free;
                }
                rc = mmu_notifier_register(&state->notifier, mm);
                mmput(mm);
                if (rc) {
                        sivfs_put_task_state(state->t_owner);
                        dout("Here");
                        goto error_free;
                }
                state->notifier_registered = true;
                if (SIVFS_DEBUG_THREADS){
                        dout("Registered mmu notifier!");
                }
        }

        //This requires workdir to be in the same file system
        //Overwrite the i_op / i_fop.
        inode->i_op = &sivfs_anon_file_inode_operations;
        inode->i_fop = &sivfs_anon_file_operations;
        inode->i_mapping->a_ops = &sivfs_anon_addr_operations;

        //Connect the anon_inode with the file_inode
        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(inode);
        iinfo->backing_inode = file_inode;
        iinfo->state = state;

        *out_inode = inode;

        if (SIVFS_DEBUG_INODES) {
                dout("Successfully created invisible file %p", inode);
        }

out:
        return rc;

error_free:
        iput(inode);

error0:
        return rc;

}

int sivfs_create_checkpoint_inode(
        struct inode** out_inode,
        struct super_block* sb
) {
        int rc = 0;

        struct inode * inode = sivfs_get_inode(
                sb,
                NULL,
                0600 | S_IFREG,
                0
        );
        if (!inode){
                rc = -ENOMEM;
                goto error0;
        }

        inode->i_op = &sivfs_checkpoint_inode_operations;
        inode->i_fop = &sivfs_checkpoint_file_operations;

        *out_inode = inode;

error0:
        return rc;
}

int sivfs_d_select_anon_inode(
        struct inode **inode_out,
        struct dentry *dentry,
        unsigned file_flags
) {
        int rc = 0;

        //Get the state associated with current, creating one if needed and
        //otherwise just incrementing the reference counter so it is not freed
        rc = sivfs_get_task_state(current);
        if (rc)
                goto error0;
        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state) {
                rc = -EPERM;
                dout("Assertion error");
                //Hmm it's a quandry whether to error0 or error1 here
                goto error0;
        }

        //We will create one anonymous invisible inode for this thread's
        //view of memory
        //
        //May start a transaction if one is not running
        //
        //Will add a refcount to state
        struct dentry* anon_dentry = NULL;
        rc = sivfs_get_anon_inode(
                &anon_dentry,
                state,
                dentry->d_inode,
                SIVFS_CREATE | SIVFS_SKIP_REFCOUNT
        );
        if (rc)
                goto error1;

        struct inode* anon_inode = anon_dentry->d_inode;

out:
        sivfs_put_task_state(current);
        *inode_out = anon_inode;
        return rc;

error1:
        sivfs_put_task_state(current);

error0:
        return rc;
}

struct inode *sivfs_d_select_inode(struct dentry *dentry, unsigned file_flags)
{
        int err;

        struct inode* file_inode = dentry->d_inode;
        //No special handling for directories. The default handling is like
        //ramfs / tmpfs, which is good enough for us.
        if (d_is_dir(dentry))
                return file_inode;

        //We use a creds() pointer comparison to see if the open desires
        //direct access to the file, or (otherwise) we create an anonymous
        //inode and transactionally manage commits to that inode

        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        struct inode* inode = NULL;
        if (current_cred() == sstate->direct_access_creds){
                inode = file_inode;
        } else {
                err = sivfs_d_select_anon_inode(
                        &inode,
                        dentry,
                        file_flags
                );

                if (err)
                        goto error0;
        }

        if (!inode){
                dout("Assertion error");
                err = -ENOMEM;
                goto error0;
        }

out:
        return inode;

error0:
        return ERR_PTR(err);
}

int sivfs_create_anon_dentry(
        struct dentry** dentry_out,
        struct inode* anon_inode
){
        int err = 0;

        struct qstr name = { .name = "sivfs-anon-file" };
        struct dentry* anon_dentry = d_alloc_pseudo(anon_inode->i_sb, &name);
        if (!anon_dentry){
                dout("Here");
                err = -ENOMEM;
                goto error0;
        }

        d_instantiate(anon_dentry, anon_inode);

out:
        *dentry_out = anon_dentry;
        return err;

error1:
        dput(anon_dentry);

error0:
        return err;
}

/*
 * OK - so the return from d_real is not really "reference counted" as it ought
 * to be. That being said, d_real is used in very few places where there is not
 * also a live file reference (which can be used for reference counting.) These
 * are:
 *  - in vfs_open, right before calling do_dentry_open, which will soon call
 *  our vfs open method - the create inode can't be destroyed in between
 *  - vfs_truncate, right before calling do_truncate, the created inode isn't
 *  destroyed by a truncate operation.
 *
 * So, the assumption is that the return of d_real is guaranteed - at least on
 * this thread of execution - to not have a file removed before it is used.
 *
 * But what if another thread destroys our file? I.e. it is snooping in our
 * mmaps. Then the file remove could come in in the window between d_real
 * returning and an operation on that inode being used.
 *
 * The way we handle this now is we clear the iinfo field whenever we are
 * freeing the state because of closing a file, operations on that inode will
 * fail. That should never happen, unless a very weird interleaving occurs, but
 * in that case, we just return error and are safe.
 */
struct dentry *sivfs_d_real(
        struct dentry *dentry,
        const struct inode *inode,
        unsigned int open_flags
)
{
        int err = 0;

        if (d_is_dir(dentry)) {
                //Hmm...
                if (!inode || inode == d_inode(dentry))
                        return dentry;

                //Getting here is apparently an assertion error?
                dout("Assertion error");
                err = -EINVAL;
                goto error0;
        }

        //Handle direct_access_creds case:
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;
        if (current_cred() == sstate->direct_access_creds){
                return dentry;
        }

        //Otherwise we are opening in anonymous mode:
        if (d_is_negative(dentry)) {
                err = -EINVAL;
                goto error0;
        }

        if (open_flags && inode){
                //Documentation says this never happens
                dout("Assertion error");
                err = -EINVAL;
                goto error0;
        }

        dout("d_real dentry %p open_flags %d", dentry, open_flags);

        if (!dentry->d_sb){
                dout("Here");
                err = -EINVAL;
                goto error0;
        }

        //TODO have get_task_state returned the got-state
        err = sivfs_get_task_state(current);
        if (err)
                goto error0;
        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state) {
                err = -EPERM;
                dout("Assertion error");
                //Hmm it's a quandry whether to error0 or error1 here
                goto error0;
        }

        //We will create one anonymous invisible inode for this thread's
        //view of memory
        //
        //May start a transaction if one is not running
        //
        //Will add a refcount to state
        struct dentry* anon_dentry = NULL;
        err = sivfs_get_anon_inode(
                &anon_dentry,
                state,
                dentry->d_inode,
                SIVFS_CREATE | SIVFS_SKIP_REFCOUNT
        );
        if (err)
                goto error1;

        dout("d_real returning %p", anon_dentry);

out:
        sivfs_put_task_state(current);
        return anon_dentry;

error1:
        sivfs_put_task_state(current);

error0:
        return ERR_PTR(err);
}

//Aops for everything but anonymous files. I.e. this is uninteresting.
const struct address_space_operations sivfs_aops = {
        .readpage       = simple_readpage,
        .write_begin    = simple_write_begin,
        .write_end      = simple_write_end,
        .set_page_dirty = sivfs_set_page_dirty
};

//page is locked on call
int sivfs_readpage_error(struct file *file, struct page *page){
        dout("Request to read page %zd of file %zd", page->index, file->f_inode->i_ino);

        //We MUST unlock the page, even if erroring.
        unlock_page(page);
        return -EINVAL;
}

//Aops for the anonymous files. Interesting!
const struct address_space_operations sivfs_anon_addr_operations = {
        .writepage	= sivfs_writepage,
        //.readpage       = sivfs_readpage_error,
        .readpage       = simple_readpage,
        .write_begin    = simple_write_begin,
        .write_end      = simple_write_end,
        .set_page_dirty = sivfs_set_page_dirty
};

//Creates an inode with owner dir under dev dev
//but does NOT link the inode into the directory tree
//so this can be used to create invisible inodes
//Returns NULL on failure
struct inode *sivfs_get_inode(
        struct super_block *sb,
        const struct inode *dir,
        umode_t mode,
        dev_t dev
) {
        struct inode * inode = new_inode(sb);

        if (!inode)
                goto error0;

        //Allocate space for the priv
        struct sivfs_inode_info* iinfo;
        int rc = sivfs_new_iinfo(&iinfo);
        if (rc)
                goto error1;

        inode->i_private = iinfo;

        //This is too verbose unless we really want debug information for each
        //checkpoint inode
        if (SIVFS_DEBUG_INODES && SIVFS_DEBUG_MAKE_CHECKPOINTS) {
                dout("Created inode %p with mode %o", inode, mode);
        }

        inode->i_ino = get_next_ino();
        inode_init_owner(inode, dir, mode);
        inode->i_mapping->a_ops = &sivfs_aops;
        mapping_set_gfp_mask(inode->i_mapping, GFP_USER);
        //Actually, we should be able to weaken to HIGHUSER but whatever we don't care about
        //architectures where highmem is a component, for now. see PORTING.
        //mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
        //mapping_set_unevictable(inode->i_mapping);
        inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
        switch (mode & S_IFMT) {
        default:
                init_special_inode(inode, mode, dev);
                break;
        case S_IFREG:
                inode->i_op = &sivfs_file_inode_operations;
                inode->i_fop = &sivfs_file_operations;
                break;
        case S_IFDIR:
                inode->i_op = &sivfs_dir_inode_operations;
                //inode->i_fop = &simple_dir_operations;
                inode->i_fop = &sivfs_dir_operations;

                /* directory inodes start off with i_nlink == 2
                (for "." entry) */
                inc_nlink(inode);
                break;
        case S_IFLNK:
                inode->i_op = &page_symlink_inode_operations;
                break;
        }

out:
        //Good out
        return inode;

error1:
        //Just need an input to undo new_inode
        iput(inode);
        inode = NULL;

error0:
        return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
//Makes a new inode and links it into the directory tree under a given device
static int
sivfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
        struct inode * inode = sivfs_get_inode(dir->i_sb, dir, mode, dev);
        int error = -ENOSPC;
        //Gets here fine
        if (inode) {
                d_instantiate(dentry, inode);
                dget(dentry);   /* Extra count - pin the dentry in core */
                error = 0;
                dir->i_mtime = dir->i_ctime = CURRENT_TIME;
        }
        return error;
}

//Make a directory inode and link into directory tree under dev 0
static int sivfs_mkdir(
        struct inode * dir,
        struct dentry * dentry,
        umode_t mode
){
        int retval = sivfs_mknod(dir, dentry, mode | S_IFDIR, 0);
        if (!retval)
                inc_nlink(dir);
        return retval;
}

//Make a inode and link into directory tree under dev 0
int sivfs_create(
        struct inode *dir,
        struct dentry *dentry,
        umode_t mode,
        bool excl
){
        return sivfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

//Create a symlink and link into directory tree under dev 0
static int sivfs_symlink(
        struct inode * dir,
        struct dentry *dentry,
        const char * symname
){
        struct inode *inode;
        int error = -ENOSPC;

        inode = sivfs_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
        if (inode) {
                int l = strlen(symname)+1;
                error = page_symlink(inode, symname, l);
                if (!error) {
                        d_instantiate(dentry, inode);
                        dget(dentry);
                        dir->i_mtime = dir->i_ctime = CURRENT_TIME;
                } else
                        iput(inode);
        }
        return error;
}

const struct inode_operations sivfs_dir_inode_operations = {
        .create         = sivfs_create,
        .lookup         = simple_lookup,
        .link           = simple_link,
        .unlink         = simple_unlink,
        .symlink        = sivfs_symlink,
        .mkdir          = sivfs_mkdir,
        .rmdir          = simple_rmdir,
        .mknod          = sivfs_mknod,
        .rename         = simple_rename,
};

const struct file_operations sivfs_dir_operations = {
        .open           = dcache_dir_open,
        .release        = dcache_dir_close,
        .llseek         = dcache_dir_lseek,
        .read           = generic_read_dir,
        .iterate        = dcache_readdir,
        .fsync          = noop_fsync,
};

const struct file_operations sivfs_commit_file_operations = {
        .read_iter      = generic_file_read_iter,
        .write_iter     = generic_file_write_iter,
};

const struct file_operations sivfs_file_operations = {
        .read_iter      = generic_file_read_iter,
        .write_iter     = generic_file_write_iter,
        .mmap           = generic_file_mmap,
        .fsync          = noop_fsync,
        .splice_read    = generic_file_splice_read,
        .splice_write   = iter_file_splice_write,
        .llseek         = generic_file_llseek,
        //.release        = sivfs_release,
};

//Called with i_mutex on dentry->d_inode held.
int sivfs_setattr(struct dentry* dentry, struct iattr* iattr){
        dout("In sivfs_setattr");

        int rc = 0;

        //Extracted from simple_setattr

        //This is the backing inode, not an anon inode.
        struct inode *inode = d_inode(dentry);

        //This changed from inode_change_ok to setattr_prepare in 4.9
#if SIVFS_COMPAT_LINUX < SIVFS_COMPAT_LINUX_4_9 
        rc = inode_change_ok(inode, iattr);
#else
        rc = setattr_prepare(dentry, iattr);
#endif
        if (rc) {
                dout("Here");
                goto error0;
        }

        if (iattr->ia_valid & ATTR_SIZE) {
                //inode is locked so this is safe:
                loff_t old_size = inode->i_size;
                loff_t new_size = iattr->ia_size;

                if (old_size != new_size){
                        rc = sivfs_get_task_state(current);
                        if (rc){
                                dout("Here");
                                goto error0;
                        }
                        struct sivfs_state* state = sivfs_task_to_state(current);
                        //Changes inode's size and commits
                        //zeros to the file, transactionally:
                        rc = sivfs_ftruncate(
                                state,
                                inode,
                                old_size,
                                new_size
                        );
                        if (rc){
                                dout("Here");
                                goto error1;
                        }
                        sivfs_put_task_state(current);
                }
        }
        setattr_copy(inode, iattr);
        mark_inode_dirty(inode);
        return rc;

error1:
        sivfs_put_task_state(current);

error0:
        return rc;
}

int sivfs_unimplemented_setattr(struct dentry* dentry, struct iattr* iattr){
        dout("In sivfs_unimplemented_setattr");
        return -EINVAL;
        //return simple_setattr(dentry, iattr);
}

const struct inode_operations sivfs_file_inode_operations = {
        .setattr        = sivfs_setattr,
        .getattr        = simple_getattr,
};


//The struct file returned by open is apparently not duplicated on fork.
const struct file_operations sivfs_anon_file_operations = {
        //.read_iter    = generic_file_read_iter,
        //.write_iter   = generic_file_write_iter,
        //
        .open           = sivfs_anon_open,
        .release        = sivfs_anon_release,
        .mmap           = sivfs_anon_mmap,
        .fsync          = noop_fsync,
        //.splice_read  = generic_file_splice_read,
        //.splice_write = iter_file_splice_write,
        //.llseek               = generic_file_llseek,
};

const struct inode_operations sivfs_anon_file_inode_operations = {
        .setattr        = sivfs_unimplemented_setattr,
        .getattr        = simple_getattr,
};

const struct inode_operations sivfs_checkpoint_inode_operations = {
        0
};

const struct file_operations sivfs_checkpoint_file_operations = {
        0
};

/*
const struct inode_operations sivfs_commit_inode_operations = {
        .setattr        = simple_setattr,
        .getattr        = simple_getattr,
};
*/
