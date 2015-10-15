#include "sivfs_nontxnal_ops.h"
#include "sivfs_common.h"
#include "sivfs_commit_update.h"

//Nontxnal ops have the unfortunate property that they update the thread's
//snapshot. So, we only allow them (for now) if the snapshot has no faulted in
//pages.
//
//Note that we could relax this restriction by somehow deferring the operation
//until the next commit. This gives calls like ftruncate() transaction-like
//semantics (i.e. aborting the transaction aborts the ftruncate, etc.) But we
//don't do that for now.
//
static int sivfs_check_nontxnal_op(
        bool* allow_out,
        struct inode* backing_inode,
        struct sivfs_state* state
){
        int rc = 0;
        bool allow = true;

        struct dentry* anon_dentry;
        rc = sivfs_get_anon_inode(
                &anon_dentry,
                state,
                backing_inode,
                0
        );
        if (rc){
                dout("Here");
                goto error0;
        }

        struct inode* anon_inode = anon_dentry->d_inode;
        struct sivfs_inode_info* iinfo = sivfs_inode_to_iinfo(anon_inode);
        if (anon_inode->i_mapping->nrpages > 0 ||
                !sivfs_radix_tree_empty(&iinfo->checkpoint_pages)
        ){
                dout("Here");
                allow = false;
                goto out;
        }

out:
        sivfs_put_anon_inode(state, backing_inode->i_ino);
        *allow_out = allow;

error0:
        return rc;
}

//lock on backing_inode must be held
int sivfs_ftruncate(
        struct sivfs_state* state,
        struct inode* backing_inode,
        loff_t old_size,
        loff_t offset
){
        int rc = 0;

        bool allow_nontxnal;
        rc = sivfs_check_nontxnal_op(&allow_nontxnal, backing_inode, state);
        if (rc){
                dout("Here");
                goto error0;
        }
        if (!allow_nontxnal){
                rc = -EINVAL;
                goto error0;
        }

        if (old_size == offset){
                dout("WARN: ftruncate called when old_size == offset");
                goto out;
        }

        //First speculatively set the filesize
        truncate_setsize(backing_inode, offset);

        /*
        if (offset >= old_size){
                //No need to commit zeros, we are extending the file.
                goto out;
        }
        */

        dout("Should commit zeros here (%016llx->%016llx)", old_size, offset);

        struct sivfs_log_entry ws;
        rc = sivfs_init_log_entry(&ws);
        if (rc){
                dout("Here");
                goto error1;
        }
        ws.ws_has_metadata = true;

        //XXX This is wrong if both offset or old_size are non 0!
        if (offset && old_size){
                dout("Not yet implemented - changing from nonempty to nonempty file");
                goto error1;
        }

        //XXX use a stream extension or something
        size_t i;
        size_t zero_range = 512*20000;
        for(i = 0; i < zero_range; ){
                size_t j;
                for( j = 0; j < 512 && i < zero_range; j++, i++){
                        struct sivfs_writeset_entry entry = {
                                .ino = backing_inode->i_ino,
                                .file_offset = i*sizeof(sivfs_word_t)
                        };

                        rc = sivfs_add_to_writeset(
                                &ws.ws,
                                &entry,
                                sizeof(entry)
                        );
                        if (rc){
                                dout("Here");
                                goto error2;
                        }
                }
                bool aborted;
                //TODO figure out whether passing 0 options here conflicts with direct
                //access transactions or whether this is a problem!
                rc = sivfs_commit_update(
                        &aborted,
                        state,
                        &ws,
                        0
                );
                if (rc){
                        dout("Here");
                        goto error2;
                }
                if (aborted){
                        dout("Assertion error - ftruncate transaction aborted!");
                        rc = -EINVAL;
                        goto error2;
                }
                //Is this necessary?
                ws.ws.__size_bytes = 0;
        }

        sivfs_destroy_log_entry(&ws);

out:
        return rc;

error2:
        sivfs_destroy_log_entry(&ws);

error1:
        //Undo the speculative size change
        truncate_setsize(backing_inode, old_size);

error0:
        return rc;
}
