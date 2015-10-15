/**
  *
  * Initialization / registration of module and filesystem
  *
  */
#include <linux/fs.h>
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
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <asm/uaccess.h>
#include <asm/tlb.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "sivfs.h"
#include "sivfs_common.h"
#include "sivfs_state.h"
#include "sivfs_shared_state.h"
#include "sivfs_writeset.h"
#include "sivfs_log.h"
#include "sivfs_commit_update.h"
#include "sivfs_mm.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Snapshot isolation virtual file system");

static struct file_system_type sivfs_fs_type = {
        .owner          = THIS_MODULE,
        .name           = "sivfs",
        .mount          = sivfs_mount,
        .kill_sb        = sivfs_kill_sb,
        .fs_flags       = 0,

        //.fs_flags       = FS_REQUIRES_DEV,

        //This is apparently very dangerous and something like a SETUID
        //.fs_flags       = FS_USERNS_MOUNT,
};

int sivfs_copy_argstruct_to_log_entry (
        struct sivfs_log_entry* log_dest,
        struct sivfs_args_t __user* arg
) {
        int rc = 0;

        //TODO IDEAS : This function gives good examples of two kinds of error
        //handling in the kernel - if (rc) goto error, and if (some condition)
        //set rc; goto error. The case: check (some condition) goto error is
        //wrong because the programmer forgot to set rc. We can try to detect
        //that last case and statically warn on it.
        struct sivfs_args_t shallow;
        rc = copy_from_user(&shallow, arg, sizeof(shallow));
        if (rc) {
                rc = -EFAULT;
                goto error0;
        }

        //Convenience variable for the destination
        struct sivfs_writeset* ws = &log_dest->ws;

        //Copy shallow's buffer of entries
        if (sivfs_ws_is_disabled(&shallow.ws)){
                //More specific error for disabled ws in direct access
                //we should never get here in direct access
                dout(
                        "ERROR: Attempt to commit during direct access"
                );
                rc = -EFAULT;
                goto error0;
        }
        //TODO also check this inside commit_update
        if (shallow.ws.__size_bytes > MAX_COMMIT_SIZE){
                dout(
                        "Large commit (%zd)! Increase MAX_COMMIT_SIZE",
                        shallow.ws.__size_bytes
                );
                rc = -EFAULT;
                goto error0;
        }
        rc = sivfs_writeset_reserve(ws, shallow.ws.__size_bytes);
        if (rc)
                goto error0;
        rc = copy_from_user(
                ws->entries,
                (struct sivfs_writeset_entry __user *) shallow.ws.entries,
                shallow.ws.__size_bytes
        );
        if (rc) {
                rc = -EFAULT;
                goto error0;
        }
        ws->__size_bytes = shallow.ws.__size_bytes;

        //Start initially not aborted and with no metadata
        log_dest->aborted = false;
        log_dest->ws_has_metadata = false;

error0:
        return rc;
}

static int sivfs_ioctl_commit_update(unsigned long arg) {
        int rc = 0;

        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state){
                dout("No open sivfs files, skipping commit/update");
                //This is an OK situation, we can return 0, but returning error
                //just to help debug unexpected behavior (for now.)
                rc = -ENOMEM;
                goto error0;
        }
        //What if state is freed below here? Might need to bump refcount.

        //Shallow copy of args used only for returning results
        struct sivfs_args_t argstruct_shallow;

        rc = copy_from_user(
                &argstruct_shallow,
                (struct sivfs_args_t __user *) arg,
                sizeof(argstruct_shallow)
        );
        if (rc){
                dout("Here");
                rc = -EFAULT;
                goto error0;
        }

        //Use temporary storage to avoid allocation
        struct sivfs_log_entry* log = &state->tmp_log_writeset;

        //initializes argstruct, requires undo with sivfs_destroy_argstruct
        rc = sivfs_copy_argstruct_to_log_entry(
                log,
                (struct sivfs_args_t __user *) arg
        );
        if (rc) {
                dout("Here");
                goto error0;
        }

        int options = 0;
        if (argstruct_shallow.abort) options |= SIVFS_ABORT_FLAG;
        if (argstruct_shallow.wants_direct_access) options |= SIVFS_DIRECT_ACCESS;

        //Modify argstruct_shallow to serve return
        rc = sivfs_commit_update(
                &argstruct_shallow.aborted,
                state,
                log,
                options
        );
        if (rc) {
                dout("Here");
                goto error0;
        }

        //Set ws size to 0 as an additional indicator of success
        //(we do this on abort or commit.)
        argstruct_shallow.ws.__size_bytes = 0;
        rc = copy_to_user(
                        (struct sivfs_args_t __user *) arg,
                        &argstruct_shallow,
                        sizeof(argstruct_shallow)
        );

        if (rc) {
                dout("Here");
                rc = -EFAULT;
                goto error0;
        }

out:

error0:
        if (rc){
                dout("Error in commit %d\n", rc);
        }

        return rc;
}

static int sivfs_ioctl_txn_pause(void){
        int rc = 0;

        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state){
                dout("No open sivfs files, skipping txn pause");
                //This is an OK situation, we can return 0, but returning error
                //just to help debug unexpected behavior (for now.)
                rc = -ENOMEM;
                goto error0;
        }

        rc = sivfs_invalidate_workingset(&state->workingset, 0);
        if (rc)
                goto error0;

        //Can now checkout nothing. Order ensures state->ts should always be >= snapshot_ts
        //though it probably doesn't matter
        state->ts = SIVFS_INVALID_TS;
        atomic64_set(&state->checkpoint_ts, SIVFS_INVALID_TS);
        atomic64_set(&state->prior_checkpoint_ts, SIVFS_INVALID_TS);
        state->checkpoint = NULL;

out:

error0:
        return rc;
}

static int sivfs_ioctl_get_stats(unsigned long arg) {
        int rc = 0;

        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        //Shallow copy of args used only for returning results
        struct sivfs_args_t argstruct_shallow;

        rc = copy_from_user(
                &argstruct_shallow,
                (struct sivfs_args_t __user *) arg,
                sizeof(argstruct_shallow)
        );
        if (rc){
                dout("Here");
                rc = -EFAULT;
                goto error0;
        }

        //Update global stats snapshot
        sivfs_update_stats();

        //Copy global stats snapshot into userspace
        rc = copy_to_user(
                (struct sivfs_stats __user *) argstruct_shallow.stats,
                &sstate->stats,
                sizeof(struct sivfs_stats)
        );

        if (rc) {
                dout("Here");
                rc = -EFAULT;
                goto error0;
        }

out:

error0:
        if (rc){
                dout("Error getting stats %d\n", rc);
        }

        return rc;
}

//Gets page info for a virtual address
int sivfs_get_page_info_current(
        struct sivfs_page_info* info,
        unsigned long addr
){
        int rc = 0;

        //Lock current mm
        struct mm_struct *mm = current->mm;
        down_read(&mm->mmap_sem);

        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        struct vm_area_struct* vma;
        rc = sivfs_find_vma(&vma, sstate, addr, 1, false);

        if (rc){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        bool is_page_file_backed = false;
        bool is_vma_file_backed = false;
        bool is_checkpoint_page = false;
        bool is_invalid_mapcount = false;
        bool bdi_accounts_writeback = false;
        bool is_page_unevictable = false;
        bool is_checkpoint_mapped = false;
        bool is_page_dirty = false;

        if (vma->vm_file){
                is_vma_file_backed = true;
                bdi_accounts_writeback = mapping_cap_account_dirty(vma->vm_file->f_mapping);
        }

        struct mmu_gather tlb;
        unsigned long end = addr + 1;

        //Some assertion checks - the range should be a sivfs managed range
        //The range will NOT be PFNMAP
        if(vma->vm_flags & VM_PFNMAP){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        //Huh, not supported for now?
        if (is_vm_hugetlb_page(vma)){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        //unsigned long next;
        swp_entry_t entry = {.val = 0};
        struct page *page = NULL;
        pte_t ptent = {0};
        pud_t* pud = NULL;
        pmd_t* pmd = NULL;

        bool lazy_mmu_mode = false;
        bool pte_map_locked = false;

        pgd_t* pgd = pgd_offset(mm, addr);
        //next = pgd_addr_end(addr, end);
        if (pgd_none(*pgd) || pgd_bad(*pgd)){
                goto out;
        }

        pud = pud_offset(pgd, addr);
        //next = pud_addr_end(addr, end);
        if (pud_none(*pud)){
                goto out;
        }

        if (pud_bad(*pud)){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        pmd = pmd_offset(pud, addr);
        if (pmd_trans_huge(*pmd)){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        if (pmd_none(*pmd)){
                goto out;
        }

        if (pmd_bad(*pmd)){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        struct page* pmd_page = pfn_to_page(pmd_pfn(*pmd));
        if (pmd_page && sivfs_is_checkpoint_page(pmd_page)){
                //Simple check for whether we have a checkpoint checked out
                is_checkpoint_mapped = true;
        }

        //int rss[NR_MM_COUNTERS];
        spinlock_t *ptl;
        pte_t *start_pte;
        pte_t *pte;

        //init_rss_vec(rss);

        //Thorny issues arise when trying to derefernce ptes to a page,
        //just grab the lock and be done with it.
        start_pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
        if (!start_pte){
                goto out;
        }

        pte_map_locked = true;
        pte = start_pte;

        //Arch_lazy mmu_mode is used for batching TLB invalidations, but
        //we don't actually change any ptes below.

        ptent = *pte;

        if (pte_none(ptent)) {
                goto out;
        }

        if (pte_present(ptent)) {
                //The next two lines might not be correct.
                unsigned long pfn = pte_pfn(ptent);
                page = pfn_to_page(pfn);

                if (unlikely(!page)){
                        dout("Here");
                        rc = -EINVAL;
                        goto error_ptl_unlock;
                }

                if (!PageAnon(page)) {
                        is_page_file_backed = true;
                }

                if (PageUnevictable(page)){
                        is_page_unevictable = true;
                }

                if (PageDirty(page)){
                        is_page_dirty = true;
                }

                is_checkpoint_page = sivfs_is_checkpoint_page(page);

                if (unlikely(page_mapcount(page) < 0)){
                        is_invalid_mapcount = true;
                }
        } else {
                entry = pte_to_swp_entry(ptent);
        }

out:
        info->address = addr;
        info->pte_path[0] = pgd->pgd;
        info->pte_path[1] = pud ? pud->pud : 0;
        info->pte_path[2] = pmd ? pmd->pmd : 0;
        info->pte_path[3] = ptent.pte;

        info->pte_flags =
        pte_write(ptent)?(
                pte_dirty(ptent)?"WD":"W")
        :(
                /*pte_read(ptent)?"R":*/(
                        pte_present(ptent)?"P":"N"
                )
        );

        info->swp_ent = entry.val;
        info->page = page;
        info->is_page_file_backed = is_page_file_backed;
        info->is_vma_file_backed = is_vma_file_backed;
        info->is_checkpoint_page = is_checkpoint_page;
        info->is_invalid_mapcount = is_invalid_mapcount;
        info->bdi_accounts_writeback = bdi_accounts_writeback;
        info->is_page_unevictable = is_page_unevictable;
        info->is_checkpoint_mapped = is_checkpoint_mapped;
        info->is_dirty = is_page_dirty;

error_ptl_unlock:
        if (pte_map_locked){
                pte_unmap_unlock(start_pte, ptl);
        }

error0:
        up_read(&mm->mmap_sem);
        return rc;
}

static int sivfs_ioctl_get_page_info(unsigned long arg) {
        int rc = 0;

        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        //Shallow copy of args used only for returning results
        struct sivfs_args_t argstruct_shallow;

        rc = copy_from_user(
                &argstruct_shallow,
                (struct sivfs_args_t __user *) arg,
                sizeof(argstruct_shallow)
        );
        if (rc){
                dout("Here");
                rc = -EFAULT;
                goto error0;
        }

        struct sivfs_page_info info;
        rc = sivfs_get_page_info_current(
                &info,
                (unsigned long) argstruct_shallow.target_address
        );
        if (rc){
                dout("Here");
                goto error0;
        }

        //Don't need to zero
        size_t buf_size = 256;
        char* buf = kmalloc(buf_size, GFP_KERNEL);

        int prc = sivfs_page_info_to_string(buf, buf_size, &info);

        if (prc < 0){
                //Encoding error
                dout("Here");
                rc = -EINVAL;
                goto error_free;
        }

        //For the next few lines, note that prc does not include the nullterm

        if (prc >= buf_size){
                //Not large enough to fit in kernel buffer
                dout("Here");
                rc = -EINVAL;
                goto error_free;
        }

        if (prc >= argstruct_shallow.buf_size){
                //Not large enough to fit in user provided buffer
                dout("Here");
                rc = -EFAULT;
                goto error_free;
        }

        //Success, copy it over
        rc = copy_to_user(
                (struct sivfs_stats __user *) argstruct_shallow.buf,
                buf,
                prc + 1
        );
        if (rc){
                //Invalid user provided buffer
                dout("Here");
                rc = -EFAULT;
                goto error_free;
        }


error_free:
        kfree(buf);

error0:
        if (rc){
                dout("Error getting stats %d\n", rc);
        }

        return rc;
}

static int sivfs_ioctl_alloc_block(unsigned long arg) {
        int rc = 0;
        
        bool mmap_sem_locked = false;

        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state){
                //The mm search below cannot succeed if we don't have a state
                //i.e. have at least one open vma
                rc = -EINVAL;
                goto error0;
        }

        //Shallow copy of args used only for returning results
        struct sivfs_allocator_args_t argstruct_shallow;
        rc = copy_from_user(
                &argstruct_shallow,
                (struct sivfs_allocator_args_t __user *) arg,
                sizeof(argstruct_shallow)
        );
        if (rc){
                rc = -EFAULT;
                goto error0;
        }

        //Lock current mm
        struct mm_struct *mm = current->mm;
        down_read(&mm->mmap_sem);
        mmap_sem_locked = true;

        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        struct vm_area_struct* vma;
        rc = sivfs_find_vma(
                &vma,
                sstate,
                (unsigned long) argstruct_shallow.zone,
                1,
                true
        );

        if (rc){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_inode_info* backing_iinfo = sivfs_vma_to_backing_iinfo(vma);
        if (!backing_iinfo){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        size_t requested_alloc = argstruct_shallow.block_alloc;
        //block_alloc of 0 indicates don't allocate a block
        if (requested_alloc != 0){
                struct sivfs_allocator_info* ainfo;
                rc = sivfs_iinfo_get_alloc_info(
                        &ainfo,
                        backing_iinfo
                );
                if (rc){
                        dout("Here");
                        rc = -EINVAL;
                        goto error0;
                }

                size_t alloc_ret_foffset;
                rc = sivfs_alloc_block(
                        &alloc_ret_foffset,
                        &argstruct_shallow.alloc_size,
                        state,
                        ainfo,
                        argstruct_shallow.block_alloc
                );
                if (rc){
                        dout("Here");
                        rc = -EINVAL;
                        goto error0;
                }

                //Convert to virtual address of vma
                argstruct_shallow.alloc_ret =
                        (void*)(vma->vm_start + alloc_ret_foffset)
                ;
        }
        argstruct_shallow.mmap_start = (void*) vma->vm_start;
        argstruct_shallow.mmap_size = vma->vm_end - vma->vm_start;

        //Success, copy it over
        rc = copy_to_user(
                (struct sivfs_args_t __user *) arg,
                &argstruct_shallow,
                sizeof(argstruct_shallow)
        );
        if (rc){
                //Invalid user provided buffer
                dout("Here");
                rc = -EFAULT;
                goto error_free;
        }

error_free:
        //Free the allocation
        //Note that if requested_alloc == 0 no allocation ocurred
        if (requested_alloc != 0){
                //TODO
        }

error0:
        if (mmap_sem_locked){
                up_read(&mm->mmap_sem);
        }
        return rc;
}

static int sivfs_ioctl_free_block(unsigned long arg) {
        int rc = 0;
        
        bool mmap_sem_locked = false;

        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state){
                //The mm search below cannot succeed if we don't have a state
                //i.e. have at least one open vma
                rc = -EINVAL;
                goto error0;
        }

        //Shallow copy of args used only for returning results
        struct sivfs_allocator_args_t argstruct_shallow;
        rc = copy_from_user(
                &argstruct_shallow,
                (struct sivfs_allocator_args_t __user *) arg,
                sizeof(argstruct_shallow)
        );
        if (rc){
                rc = -EFAULT;
                goto error0;
        }

        //Lock current mm
        struct mm_struct *mm = current->mm;
        down_read(&mm->mmap_sem);
        mmap_sem_locked = true;

        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        struct vm_area_struct* vma;
        rc = sivfs_find_vma(
                &vma,
                sstate,
                (unsigned long) argstruct_shallow.zone,
                1,
                true
        );

        if (rc){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_inode_info* backing_iinfo = sivfs_vma_to_backing_iinfo(vma);
        if (!backing_iinfo){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        //Use "zone" as target to free
        size_t file_offset =
                ((unsigned long)argstruct_shallow.zone - vma->vm_start) +
                ((size_t)vma->vm_pgoff << PAGE_SHIFT)
        ;

        struct sivfs_allocator_info* ainfo;
        rc = sivfs_iinfo_get_alloc_info(
                &ainfo,
                backing_iinfo
        );
        if (rc){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        rc = sivfs_free_block(
                state,
                ainfo,
                file_offset
        );
        if (rc){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        //Success.
error0:
        if (mmap_sem_locked){
                up_read(&mm->mmap_sem);
        }
        return rc;
}

static long sivfs_ioctl(
        struct file *filp,
        unsigned int ioctl,
        unsigned long arg
) {
        long rc = 0;

        switch(ioctl) {
                case SIVFS_TXN_COMMIT_UPDATE:
                        rc = sivfs_ioctl_commit_update(arg);
                        break;

                case SIVFS_TXN_PAUSE:
                        rc = sivfs_ioctl_txn_pause();
                        break;

                case SIVFS_GET_STATS:
                        rc = sivfs_ioctl_get_stats(arg);
                        break;

                case SIVFS_GET_PAGE_INFO:
                        rc = sivfs_ioctl_get_page_info(arg);
                        break;

                case SIVFS_ALLOC_BLOCK:
                        rc = sivfs_ioctl_alloc_block(arg);
                        break;

                case SIVFS_FREE_BLOCK:
                        rc = sivfs_ioctl_free_block(arg);
                        break;

                case SIVFS_ALLOC_FREE_LIST:
                        dout("Not yet implemented");
                        rc = -EINVAL;
                        break;

                case SIVFS_FREE_FREE_LIST:
                        dout("Not yet implemented");
                        rc = -EINVAL;
                        break;

                default:
                        dout("Unrecognized ioctl on /dev/sivfs");
                        //Unrecognized request
                        rc = -EINVAL;
                        break;
        }

        return rc;
}


//Device operations on /dev/sivfs
struct file_operations sivfs_chardev_ops = {
        .owner          = THIS_MODULE,
        .unlocked_ioctl = sivfs_ioctl,
#ifdef CONFIG_COMPAT
        //???
        //.compat_ioctl = timing_defense_dev_ioctl,
#endif
//      .llseek         = noop_llseek,
//      .mmap           = my_mmap,
};

static struct miscdevice sivfs_dev = {
        MISC_DYNAMIC_MINOR,
        "sivfs",
        &sivfs_chardev_ops,
};

int __init init_sivfs_fs(void)
{
        int ret = 0;

        //Now publicize the filesystem
        ret = misc_register(&sivfs_dev);
        if (ret)
                goto error_0;

        ret = register_filesystem(&sivfs_fs_type);
        if (ret)
                goto error_1;

        //Emit this as a PRINTK so it looks visually distinct
        printk(KERN_INFO "sivfs module inited");
        dout("sivfs module inited");

out:
        return ret;

error_1:
        misc_deregister(&sivfs_dev);

error_0:
        return ret;
}
//fs_initcall(init_sivfs_fs);

/*
 * Unregister the SIVFS filesystems
 * (assumes init_sivfs_fs returned successfully before hand)
 */
void __exit exit_sivfs_fs(void)
{
        //There should be no open sivfs mounts at this point

        dout("Exiting sivfs");

        misc_deregister(&sivfs_dev);
        unregister_filesystem(&sivfs_fs_type);

        dout("sivfs module uninited");
        //Emit this as a PRINTK so it looks visually distinct
        printk(KERN_INFO "sivfs module uninited");
}

module_init(init_sivfs_fs);
module_exit(exit_sivfs_fs);
