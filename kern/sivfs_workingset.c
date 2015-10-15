#include "sivfs.h"
#include "sivfs_state.h"
#include "sivfs_workingset.h"
#include "sivfs_radix_tree.h"
#include "sivfs_commit_update.h"
#include "sivfs_mm.h"

//anon inodes always have a pseudo directory entry pointing to them

//Helper for sivfs_get_anon_inode, does the path where we don't have an anon
//inode yet
int _sivfs_get_anon_inode(
        struct dentry** dentry_out,
        struct sivfs_state* state,
        struct inode* backing_inode,
        int flags
){
        int rc = 0;
        struct inode* anon_inode = NULL;
        bool should_inc_refcount = !(flags & SIVFS_SKIP_REFCOUNT);
        struct sivfs_workingset* ws = &state->workingset;
        unsigned long backing_ino = backing_inode->i_ino;

        //Should we create one?
        if (!(flags & SIVFS_CREATE)){
                //Just return indicating no entry
                goto out;
        }

        //Put a reference on state. We remove this in put_anon_inode
        rc = sivfs_get_task_state(state->t_owner);
        if (rc){
                dout("Here");
                goto error0;
        }

        //Create new anon inode for state. Comes with ref 1.
        rc = sivfs_create_anon_inode(
                &anon_inode,
                backing_inode,
                state
        );
        if (rc)
                goto error1;

        struct sivfs_inode_info* iinfo =
                sivfs_inode_to_iinfo(anon_inode)
        ;
        if (!iinfo){
                dout("Assertion error");
                rc = -EINVAL;
                goto error2;
        }

        //Bit of a hack to correctly set initial refcount:
        if (!should_inc_refcount){
                iinfo->refcount = SIVFS_UNREFERENCED;
        }

        bool anon_inodes_was_empty = sivfs_radix_tree_empty(&ws->anon_inodes);
        //Insert it.
        //We can't race on insertion, since state is owned by current
        rc = radix_tree_insert(
                &ws->anon_inodes,
                backing_ino,
                anon_inode
        );
        if (rc == -EEXIST){
                dout("Assertion error: state already had anon inode");
        }
        if (rc)
                goto error2;

        struct dentry* anon_dentry;
        rc = sivfs_create_anon_dentry(
                &anon_dentry,
                anon_inode
        );
        if (rc)
                goto error3;

        //Insert it.
        //We can't race on insertion, since state is owned by current
        rc = radix_tree_insert(
                &ws->anon_dentries,
                backing_ino,
                anon_dentry
        );
        if (rc == -EEXIST){
                dout("Assertion error: state already had anon inode");
        }
        if (rc)
                goto error4;

        //If we had no previous anon inodes, call update
        //Really we could defer this all the way up to the first file
        //access, but let's do it here to catch errors early
        if (anon_inodes_was_empty){
                bool aborted = false;
                rc = sivfs_commit_update(
                        &aborted,
                        state,
                        NULL,
                        0
                );
                if (rc)
                        goto error5;
                if (aborted){
                        dout("Assertion error");
                        goto error5;
                }
        }
out:
        *dentry_out = anon_dentry;
        return rc;

error5:
        radix_tree_delete(&ws->anon_dentries, backing_ino);

error4:
        dput(anon_dentry);
        //Null out anon_inode here to avoid the iput below
        anon_inode = NULL;

error3:
        radix_tree_delete(&ws->anon_inodes, backing_ino);

error2:
        if (anon_inode){
                iput(anon_inode);
        }

error1:
        sivfs_put_task_state(state->t_owner);

error0:
        return rc;
}

int sivfs_get_anon_inode(
        struct dentry** dentry_out,
        struct sivfs_state* state,
        struct inode* backing_inode,
        int flags
)
{
        int rc = 0;

        bool should_inc_refcount = !(flags & SIVFS_SKIP_REFCOUNT);

        struct sivfs_workingset* ws = &state->workingset;

        unsigned long backing_ino = backing_inode->i_ino;

        //Do we already have one?
        struct dentry* anon_dentry = radix_tree_lookup(
                &ws->anon_dentries,
                backing_ino
        );
        if (anon_dentry){
                if (should_inc_refcount){
                        struct inode* anon_inode = anon_dentry->d_inode;

                        //Increment refcount and return it.
                        struct sivfs_inode_info* iinfo =
                                sivfs_inode_to_iinfo(anon_inode)
                        ;
                        if (!iinfo){
                                dout("Assertion error");
                                rc = -EINVAL;
                                goto error0;
                        }

                        //Handle UNREFERENCED refcount
                        if (iinfo->refcount == SIVFS_UNREFERENCED){
                                iinfo->refcount = 1;
                        } else {
                                rc = sivfs_iinfo_refct_inc(iinfo);
                                if (rc){
                                        //Can occur due to too many mmaps
                                        goto error0;
                                }
                        }
                }
        } else {
                rc = _sivfs_get_anon_inode(
                        &anon_dentry,
                        state,
                        backing_inode,
                        flags
                );
                if (rc){
                        dout("Here");
                        goto error0;
                }
        }

out:
        *dentry_out = anon_dentry;
        return rc;

error0:
        return rc;
}

void sivfs_put_anon_inode(
        struct sivfs_state* state,
        unsigned long backing_ino
){
        struct sivfs_workingset* ws = &state->workingset;
        struct inode* anon_inode = radix_tree_lookup(&ws->anon_inodes, backing_ino);
        if (!anon_inode){
                dout("BUG - put_anon_inode called without an anon_inode");
                return;
        }

        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(anon_inode);
        if (!iinfo){
                dout("BUG - put_anon_inode called without an iinfo");
                return;
        }

        //Handle unreferenced special case:
        if (iinfo->refcount == SIVFS_UNREFERENCED){
                dout("WARN - removing an unreferenced anon inode");
                //OK! free the element.
        } else if (iinfo->refcount == 1){
                //OK! free the element.
        } else if (iinfo->refcount < 1){
                dout("ERROR - Invalid refcount on anon_inode %d", iinfo->refcount);
                return;
        } else {
                //Otherwise just decrement the refcount
                iinfo->refcount--;
                return;
        }
        //Free the iinfo
        sivfs_put_iinfo(iinfo);
        anon_inode->i_private = NULL;

        dout("Freeing anon inode %p!", anon_inode);
        struct inode* _anon_inode = radix_tree_delete(&ws->anon_inodes, backing_ino);
        if (_anon_inode != anon_inode){
                dout("Error - anon_dentry was not in anon_dentries!");
        }

        struct dentry* anon_dentry = radix_tree_delete(&ws->anon_dentries, backing_ino);
        if (!anon_dentry){
                dout("BUG - anon_inode did not have an anon_dentry!");
                return;
        }
        //This dput will also decrement the refcount on anon_inode
        dput(anon_dentry);

        //Finally, put the reference on state
        sivfs_put_task_state(state->t_owner);
}

//Finds and gets a working set page containing offset. Offset need not be page
//aligned.
//If a checkpoint page is mapped in and flags is set to EVICT_CHECKPOINT,
//tries to evicts the checkpoint page and NULL is returned. Otherwise, fails with an
//error if a checkpoint page is mapped in.
//Otherwise, returns working set page with its refcount increased, or NULL if no mapped in page.
int sivfs_find_get_wspg(
        struct page** out_pg,
        struct sivfs_state* state,
        struct sivfs_workingset* ws,
        unsigned long backing_ino,
        size_t offset,
        int flags
){
        int rc = 0;
        struct page* page = NULL;
        pgoff_t pgoff = offset >> PAGE_SHIFT;

        //TODO rcu_read_lock needed here because one thread could call update
        //simultaneously while another closes a file handle and unmaps the file
        //leading to the radix_tree_delete in put_anon_inode
        struct inode* anon_inode = sivfs_backing_ino_to_anon_inode(ws, backing_ino);
        if (!anon_inode)
                goto out;

        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(anon_inode);
        if (!iinfo){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        struct address_space* mapping = anon_inode->i_mapping;

        bool unmapped = false;

        if (iinfo->checkpoint_checked_out){
                if (flags & SIVFS_EVICT_CHECKPOINT){
                        dout("Should invalidate checkpoint");
                        sivfs_unmap_page_range(mapping, PAGE_ROUND_DOWN(offset), PAGE_SIZE);
                        iinfo->checkpoint_checked_out = false;
                        unmapped = true;
                } else {
                        rc = -EINVAL;
                        goto error0;
                }
        }

        //Yuck, we have to check if we have a checkpoint page...
        void* checkpoint_page = radix_tree_lookup(&iinfo->checkpoint_pages, pgoff);
        if (unlikely(checkpoint_page && checkpoint_page != SIVFS_RADIX_TREE_TOMBSTONE)){
                if (flags & SIVFS_EVICT_CHECKPOINT){
                        //Argument automatically rounded down to page boundaries
                        sivfs_unmap_page_range(mapping, PAGE_ROUND_DOWN(offset), PAGE_SIZE);

                        //sivfs_radix_tree_tombstone(&iinfo->checkpoint_pages, pgoff);
                        radix_tree_delete(&iinfo->checkpoint_pages, pgoff);
                        unmapped = true;
                } else {
                        rc = -EINVAL;
                        goto error0;
                }
        }

        //Lookup in the i_mapping
        page = find_get_page(mapping, pgoff);
        if (page && unmapped){
                //This shouldn't happen - we kicked the segment out above!
                dout("Assertion error - unmap_page_range didn't kick out page!");
        }

        if (page){
                goto out;
        }

        //Good out
out:
        *out_pg = page;

error0:
        return rc;
}

//Invalidates each workingset page modified by a write performed by log entry
//logent (will invalidate pages even if logent is aborted.)
//Wsents in logent must have ino and file_offset correctly filled in
int sivfs_invalidate_workingset_log(
        struct sivfs_workingset* workingset,
        struct sivfs_log_entry* logent
){
        int rc = 0;

        //Convenience
        struct sivfs_writeset* ws = &logent->ws;

        //mmap_sem needed to prevent unmapping of vmas which could modify
        //anon_ino list on state.
        struct mm_struct *mm = current->mm;
        down_read(&mm->mmap_sem);


        struct sivfs_writeset_entry* ent = ws->entries;
        size_t size_left = ws->__size_bytes;
        while(size_left){
                struct sivfs_wse_stream_info info;
                rc = sivfs_wse_to_stream_info(
                        &info,
                        ent,
                        size_left
                );
                if (rc){
                        dout("Here");
                        goto error0;
                }

                struct inode* anon_inode =
                        sivfs_backing_ino_to_anon_inode(workingset, ent->ino)
                ;

                if (anon_inode){
                        struct address_space *mapping = anon_inode->i_mapping;
                        loff_t unmap_start =
                                PAGE_ROUND_DOWN(ent->file_offset)
                        ;
                        //Argument automatically rounded down to page boundaries
                        sivfs_unmap_page_range(
                                mapping,
                                unmap_start,
                                PAGE_SIZE
                                //1
                                //info.dest_words * sizeof(sivfs_word_t)
                        );
                }

                //Advance
                size_t wse_size = sivfs_wse_size(&info);
                ent = (struct sivfs_writeset_entry*)(
                        (char*)ent + wse_size
                );
                size_left -= wse_size;
        }

error0:
        up_read(&mm->mmap_sem);
        return rc;
}

//
int sivfs_invalidate_workingset(
        struct sivfs_workingset* ws,
        int options
){
        int rc = 0;

        struct mm_struct *mm = current->mm;
        down_read(&mm->mmap_sem);

        void** slot;
        struct radix_tree_iter iter;

        radix_tree_for_each_slot(
                slot,
                &ws->anon_inodes,
                &iter,
                0
        ){
                struct inode* anon_inode = *slot;

                struct address_space *mapping = anon_inode->i_mapping;

                if (SIVFS_TRACK_CLEAR_WORKINGSET_EVERY_TXN){
                        //TODO I'm not sure the below is correct anymore.
                        //Revisit.
                        dout("Implementation needs fixing");
                        rc = -EINVAL;
                        goto error0;

                        struct sivfs_inode_info* iinfo =
                                sivfs_inode_to_iinfo(anon_inode)
                        ;
                        struct sivfs_workingset_ranges_iter riter;
                        sivfs_workingset_ranges_for_each(&iinfo->anon_ranges, &riter){
                                loff_t unmap_start =
                                        PAGE_ROUND_DOWN(riter.value->start)
                                ;
                                if (unmap_start != riter.value->start){
                                        dout("Here");
                                        rc = -EINVAL;
                                        goto error0;
                                }
                                size_t size = riter.value->size;
                                //dout("Unmapping %llu", unmap_start);
                                //Unmap wants a start, size pair. Even remove COWs.
                                sivfs_unmap_page_range(mapping, unmap_start, size);
                        }

                        //Clear by setting size to 0
                        iinfo->anon_ranges.size = 0;

                } else {
                        if (options & SIVFS_NO_INVALIDATE_CPS){
                                sivfs_unmap_mapping_workingset(mapping);
                        } else {
                                sivfs_unmap_mapping(mapping);
                        }
                }
        }

error0:
        up_read(&mm->mmap_sem);

        return rc;
}
