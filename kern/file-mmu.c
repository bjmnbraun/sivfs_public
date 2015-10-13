/* file-mmu.c: sivfs MMU-based file operations
 *
 * Resizable simple ram filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *               2000 Transmeta Corp.
 *
 * Usage limits added by David Gibson, Linuxcare Australia.
 * This file is released under the GPL.
 */

/*
 * NOTE! This filesystem is probably most useful
 * not as a real filesystem, but as an example of
 * how virtual filesystems can be written.
 *
 * It doesn't get much simpler than this. Consider
 * that this file implements the full semantics of
 * a POSIX-compliant read-write filesystem.
 *
 * Note in particular how the filesystem does not
 * need to implement any data structures of its own
 * to keep track of the virtual data: using the VFS
 * caches is sufficient.
 */

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/pagemap.h>
#include <linux/sched.h>

#include "sivfs.h"
#include "sivfs_common.h"
#include "sivfs_state.h"
#include "sivfs_shared_state.h"

/**
 * sivfs_fault - read in file data for page fault handling
 * @vma:	vma in which the fault was taken
 * @vmf:	struct vm_fault containing details of the fault
 *
 * filemap_fault() is invoked via the vma operations vector for a
 * mapped memory region to read in file data during a page fault.
 *
 * The goto's are kind of ugly, but this streamlines the normal case of having
 * it in the page cache, and handles the special cases reasonably without
 * having a lot of duplicated code.
 *
 * vma->vm_mm->mmap_sem must be held on entry.
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
	int error;
	struct file *file = vma->vm_file;
	struct address_space *mapping = file->f_mapping;
	struct file_ra_state *ra = &file->f_ra;
	struct inode *inode = mapping->host;
	struct page *page;
	loff_t size;
	int ret = 0;

        //Offset, in pages
	pgoff_t offset = vmf->pgoff;
        dout("PGFault offset in file: %zx", vmf->pgoff * PAGE_SIZE);

        //Size too big?
        size = i_size_read(inode);
        if (offset >= size)
          return VM_FAULT_SIGBUS;

        //Get our per-mmap state
        struct sivfs_state* state = vma->vm_private_data;
        if (!state){
                dout("No sivfs_state for vma (!)");
                return VM_FAULT_SIGBUS;
        }

        //Concurrency check: the calling thread will remain static
        //throughout this call, see docs of sivfs_state for why this works
        if (state->task != current){
                dout("sivfs_state task differed from current");
                return VM_FAULT_SIGBUS;
        }

        //Allocate a new page to serve the fault
        //TODO check a page cache to see if we have one
        page = sivfs_allocate_local_page(state);

        //XXX
        return VM_FAULT_SIGBUS;

	/*
	 * Do we have something in the page cache already?
         *
         * Need to create a new struct page maybe?
	 */
	//page = find_get_page(mapping, offset);

	/* Did it get truncated? */
        /*
	if (unlikely(page->mapping != mapping)) {
		unlock_page(page);
		put_page(page);
		goto retry_find;
	}
	VM_BUG_ON_PAGE(page->index != offset, page);
        */

        /*
        //Race condition - can be truncated while servicing fault, in
        //which case we should fault...
        */
        //Size too big?
        size = i_size_read(inode);
        if (offset >= size)
          return VM_FAULT_SIGBUS;

        //Hmm. VM_FAULT_LOCKED seems to be returned when still holding a
        //lock on the returned struct page...
	//vmf->page = page;
	//return ret | VM_FAULT_LOCKED;

        //XXX
        ret = filemap_fault(vma, vmf);

        //if (ret == 0){
          if (!vmf->page){
            dout("HUH? No page!");
          } else {
            void* mem;
            dout("Page address: %p", vmf->page);
            //Populate the page with 42s
            mem = kmap(vmf->page);
            memset(mem, '6', PAGE_SIZE);
            kunmap(vmf->page);
          }
        //}

        return ret;
}

void sivfs_map_pages(struct vm_area_struct *vma, struct vm_fault *vmf) {
        dout("In sivfs_map_pages\n");

        //XXX
        //filemap_map_pages(vma, vmf);
}

//Copy of generic_file_vm_ops
const struct vm_operations_struct sivfs_file_vm_ops = {
        //.fault                  = filemap_fault,
        .fault                = sivfs_fault,
        //.map_pages            = filemap_map_pages,
        //.map_pages            = sivfs_map_pages,
        .page_mkwrite           = filemap_page_mkwrite,
};

int sivfs_mmap (struct file * file, struct vm_area_struct * vma){
        //Copy of generic_file_mmap
        //struct address_space *mapping = file->f_mapping;

        //Allocate some volatile state to carry around with this mmap
        //Note that this means you can have multiple mmaps of the same file,
        //the vm_area_struct becomes the handler for this particular mmapping

        //This is an operation on behalf of the user and can safely block
        //Hence GFP_KERNEL

        //TODO we should trap use a custom fault handler that does
        //filemap_fault, but on a different file.
        int ret;

        struct sivfs_state* state;
        state = kmalloc(sizeof(*state), GFP_KERNEL);
        if (!state) {
                ret = -ENOMEM;
                goto error0;
        }

        ret = init_sivfs_state(state);
        if (ret < 0)
                goto error1;

        //Mmaps in our system are per-thread, so check that the original task
        //is using the mmap on faults etc in the future:
        struct task_struct* mmaptask = current;
        state->task = mmaptask;

        vma->vm_private_data = state;

        struct sivfs_shared_state* sivss = &sivfs_shared_state;

        spin_lock(&sivss->mmap_list_lock);
        list_add_tail(&state->mmap_item, &sivss->mmap_list);
        spin_unlock(&sivss->mmap_list_lock);

        dout(
                "MMaped inode %p to PID %d\n",
                file_inode(file),
                task_pid_nr(mmaptask)
        );

        vma->vm_ops = &sivfs_file_vm_ops;
        file_accessed(file);

#if 0

        //Change the file that vma points to
        //Before this was called, the caller set vma->vm_file. Override that
        //assignment here.
        //Make an inode for the new file
        struct inode *old_inode = file->f_inode;

        struct super_block *sb = old_inode->i_sb;

        /*
           struct inode *my_inode = new_inode(sb);
           if (!my_inode){
           goto error0;
           }
         */

        //Populate the inode as a regular file
        //sivfs_get_inode(sb, my_inode, S_IFREG, 0); //hardcode dev = 0?

        struct file *old_file = vma->vm_file;
        //TODO make a macro
        dout("%s %s" "Yo yo yo%p %p\n", __FILE__, __PRETTY_FUNCTION__, old_file, file);
        struct path path;
        path.mnt = old_file->f_path.mnt;
        struct qstr qname = { .name = "", };

        //XXX
        goto error1;

        struct dentry *dentry = d_alloc_pseudo(sb, &qname);
        if (!dentry) {
                goto error1;
        }

        //Treat old_inode as a directory - I think this is safe.
        //int error = vfs_create(old_inode, dentry, S_IFREG,/*old_file->f_mode,*/ true);
        dout("Hello %p\n", sivfs_create);
        int error = sivfs_create(old_inode, dentry, S_IFREG,/*old_file->f_mode,*/ true);
        if (error){
                goto error2;
        }
        struct inode *my_inode = dentry->d_inode;
        if (!my_inode){
                goto error2;
        }

        //Properly size the inode
        dout("Old inode size is %lld\n", i_size_read(old_inode));
        truncate_setsize(my_inode, i_size_read(old_inode));

        //Associate the two in the dcache I think
#if 0
        //This works, so our inode is not yet correctly formed.
        d_instantiate(dentry, old_inode);
#else
        //d_instantiate(dentry, my_inode);
#endif
        path.dentry = dentry;

        dout("Over there! %p\n", my_inode);
        dout("Yeah2 %p %p %p\n", old_inode->i_op, my_inode->i_op, &sivfs_file_inode_operations);
        struct file *my_file = dentry_open(&path, old_file->f_flags, old_file->f_cred);
        if (IS_ERR(my_file)){
                dout("Error in dentry_open y'all! %zd\n", PTR_ERR(my_file));
                goto error2;
        }

        //O.K. now call generic mmap with the new one

        /*
           if (vma->vm_flags & VM_DENYWRITE) {
           error = deny_write_access(my_file);
           if (error)
           goto error2;
           }
           if (vma->vm_flags & VM_SHARED) {
           error = mapping_map_writable(my_file->f_mapping);
           if (error)
           goto error2;
           }
         */

        my_file = get_file(my_file);
        file_accessed(my_file);
        vma->vm_file = my_file;

        //We aren't using file any more.
        fput(file);
        /*
           if (vma->vm_flags & VM_SHARED)
           mapping_unmap_writable(file->f_mapping);
           if (vma->vm_flags & VM_DENYWRITE)
           allow_write_access(file);
         */
#endif

        return 0;

error2:
        //free_inode_nonrcu(my_inode);
        //Free dentry
        //dentry_kill(dentry);

error1:
        //free state
        kfree(state);

error0:
        return -ENOMEM;
}

const struct file_operations sivfs_file_operations = {
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	//.mmap		= generic_file_mmap,
	.mmap		= sivfs_mmap,
	.fsync		= noop_fsync,
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
};

const struct inode_operations sivfs_file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};
