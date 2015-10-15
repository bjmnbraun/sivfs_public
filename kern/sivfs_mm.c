#include "sivfs_mm.h"
#include "sivfs_shared_state.h"
#include "sivfs_state.h"

//Just kicks out normal workingset pages, i.e. not mapped in checkpoint pages.
//See sivfs_unmap_mapping.
void sivfs_unmap_mapping_workingset(struct address_space* mapping){
#if 0
        //This is a very slow way to do it
        //Unmap whole mapping, even COWs
        unmap_mapping_range(mapping, 0, 0, 1);
        //Truncate actually removes the pages
        truncate_inode_pages(mapping, 0);
#else
        //Faster than unmap_mapping, but only frees pages that are regular workingset
        //pages, i.e. non-checkpoint pages faulted in
        void** slot;
        struct radix_tree_iter iter;
        unsigned long indices[16];
        int i, nr;
        unsigned long index;

        do {
                nr = 0;
                rcu_read_lock();
                radix_tree_for_each_slot(
                        slot,
                        &mapping->page_tree,
                        &iter,
                        0
                ){
                        //Don't care about slot
                        indices[nr] = iter.index;
                        if (++nr == 16)
                                break;
                }
                rcu_read_unlock();
                for(i = 0; i < nr; i++){
                        index = indices[i];
                        sivfs_unmap_page_range(
                                mapping,
                                (index << PAGE_SHIFT),
                                PAGE_SIZE
                        );
                }
        } while (nr > 0);
#endif
}

//Kicks out all the checked-out
//checkpoint pages and workingset pages. 
void sivfs_unmap_mapping(struct address_space *mapping){
#if 0
        //This is a very slow way to do it
        //Unmap whole mapping, even COWs
        unmap_mapping_range(mapping, 0, 0, 1);
        //Truncate actually removes the pages
        truncate_inode_pages(mapping, 0);
#else
        //Kick out workingset pages
        sivfs_unmap_mapping_workingset(mapping);
        //Kick out checkpoint pages
        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(mapping->host);
        void** slot;
        struct radix_tree_iter iter;
        unsigned long indices[16];
        int i, nr;
        unsigned long index;

        do {
                nr = 0;
                rcu_read_lock();
                radix_tree_for_each_slot(
                        slot,
                        &iinfo->checkpoint_pages,
                        &iter,
                        0
                ){
                        if (*slot == SIVFS_RADIX_TREE_TOMBSTONE){
                                continue;
                        }
                        //Don't care about slot
                        indices[nr] = iter.index;
                        if (++nr == 16)
                                break;
                }
                rcu_read_unlock();
                for(i = 0; i < nr; i++){
                        index = indices[i];
                        sivfs_unmap_page_range(
                                mapping,
                                (index << PAGE_SHIFT),
                                PAGE_SIZE
                        );
                        //Remove it from the checkpoint pages tree. Do real
                        //removal here.
                        radix_tree_delete(&iinfo->checkpoint_pages, index);
                }
        } while (nr > 0);
#endif
}

int sivfs_find_vma(
        struct vm_area_struct** vma_out,
        struct sivfs_shared_state* sstate,
        unsigned long start,
        size_t size,
        bool check_sivfs_current
){
        int rc = 0;

        unsigned long end =
                start + size
        ;

        if (end < start){
                //Overflow
                rc = -EINVAL;
                goto error0;
        }
        struct vm_area_struct* vma = find_vma(
                current->mm,
                start
        );
        //find_vma is weird, we need the following checks:
        if (
                !vma ||
                vma->vm_start > start ||
                vma->vm_end < end
        ){
                rc = -EINVAL;
                goto error0;
        }

        if (check_sivfs_current){
                if (vma_is_anonymous(vma)){
                        dout("VMA was not a sivfs vma!");
                        rc = -EINVAL;
                        goto error0;
                }

                if (vma->vm_file->f_inode->i_sb != sstate->sb){
                        dout("VMA was not a sivfs vma!");
                        rc = -EINVAL;
                        goto error0;
                }

                struct sivfs_inode_info* iinfo = sivfs_vma_to_iinfo(vma);
                if (!iinfo){
                        dout("Assertion error");
                        rc = -EINVAL;
                        goto error0;
                }

                struct sivfs_state* state = sivfs_task_to_state(current);
                if (state != iinfo->state){
                        dout("Concurrency check failed - VMA not owned by current");
                        rc = -EINVAL;
                        goto error0;
                }
        }

        //Success
        *vma_out = vma;

error0:
        return rc;
}
