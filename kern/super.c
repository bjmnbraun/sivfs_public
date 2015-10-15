/**
  *
  * Handling of superblocks, aka mounts of this filesystem
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
#include <asm/uaccess.h>

#include "sivfs.h"
#include "sivfs_common.h"
#include "sivfs_shared_state.h"
#include "sivfs_config.h"


/** Parsing of options to mount command **/
enum {
        Opt_mode,
        Opt_err
};

const match_table_t tokens = {
        {Opt_mode, "mode=%o"},
        {Opt_err, NULL}
};

static int sivfs_parse_options(char *data, struct sivfs_mount_opts *opts)
{
        substring_t args[MAX_OPT_ARGS];
        int option;
        int token;
        char *p;
        int rc = 0;

        opts->mode = SIVFS_DEFAULT_MODE;

        while ((p = strsep(&data, ",")) != NULL) {
                if (!*p)
                        continue;

                token = match_token(p, tokens, args);
                switch (token) {
                case Opt_mode:
                        if (match_octal(&args[0], &option))
                                return -EINVAL;
                        opts->mode = option & S_IALLUGO;
                        break;
                /*
                * We might like to report bad mount options here;
                * but traditionally sivfs has ignored all mount options,
                * and as it is used as a !CONFIG_SHMEM simple substitute
                * for tmpfs, better continue to ignore other mount options.
                */
                }
        }

        return rc;
}

const struct super_operations sivfs_ops = {
        .statfs         = simple_statfs,
        .show_options   = generic_show_options,
        .drop_inode     = sivfs_drop_inode,
        //.evict_inode	= sivfs_evict_inode
};

static const struct dentry_operations sivfs_dentry_operations = {
        //.d_release = sivfs_dentry_release,
//Linux 4.2.1 introduced d_select_inode, Linux 4.8 replaced it with d_real
//instead. We need one of these to do our tricks, since we behave similarly to
//overlayfs
#if SIVFS_COMPAT_LINUX < SIVFS_COMPAT_LINUX_4_8
        .d_select_inode = sivfs_d_select_inode,
#else
        .d_real = sivfs_d_real,
#endif
};

struct backing_dev_info sivfs_bdi;

int sivfs_init_bdi(struct super_block *sb){
        sivfs_bdi.name = "sivfs";
        sivfs_bdi.ra_pages = (VM_MAX_READAHEAD * 1024) / PAGE_SIZE;
        //Defaults seem ok
        sivfs_bdi.capabilities = 0;

        int rc = 0;
        rc = bdi_init(&sivfs_bdi);

        if (rc) goto error0;

        rc = bdi_register_dev(&sivfs_bdi, sb->s_dev);
        if (rc) goto error0;

error0:
        return rc;
}

//Called by mount, initialize a superblock (mount) and create the top level
//directory
int sivfs_fill_super(struct super_block *sb, void *data, int silent)
{
        int rc = 0;

        dout("sivfs mounting super");
        //Emit this as a PRINTK so it looks visually distinct
        printk(KERN_INFO "sivfs mounting super");

        save_mount_options(sb, data);

        struct sivfs_mount_opts mount_opts;
        rc = sivfs_parse_options(data, &mount_opts);
        if (rc){
                goto error0;
        }

        sb->s_maxbytes          = MAX_LFS_FILESIZE;

        //For the most part, s_blocksize is only used by the vfs (i.e. our
        //code) but some helper functions like buffer_head use this to decide
        //how much to read in at a time. Reading in a page at a time makes
        //sense for our application, though this is probably never used:
        sb->s_blocksize         = PAGE_SIZE;
        sb->s_blocksize_bits    = PAGE_SHIFT;

        sb->s_magic             = SIVFS_MAGIC;
        sb->s_op                = &sivfs_ops;
        sb->s_d_op              = &sivfs_dentry_operations;
        sb->s_time_gran         = 1;

        rc = sivfs_init_bdi(sb);
        if (rc){
                goto error1;
        }

        //sb->s_bdi = &sivfs_bdi;

        //TODO allocate a fresh state for each mount
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        sb->s_fs_info = sstate;
        if (sivfs_shared_state_inited){
                dout("Multiple sivfs mounts not yet implemented.");
                goto error2;
        }
        rc = sivfs_init_shared_state(sstate, sb, &mount_opts);
        if (rc){
                goto error2;
        }

        //Make the root directory
        struct inode* inode = sivfs_get_inode(
                sb,
                NULL,
                S_IFDIR | mount_opts.mode,
                0
        );
        if (!inode){
                rc = -ENOMEM;
                goto error3;
        }

        sb->s_root = d_make_root(inode);
        if (!sb->s_root) {
                rc = -ENOMEM;
                goto error4;
        }

        dout("sivfs successfully mounted");

out:
        sivfs_shared_state_inited = true;
        return rc;

error4:
        iput(inode);

error3:
        sivfs_destroy_shared_state(sstate);

error2:
        //Cleanup bdi
        bdi_destroy(&sivfs_bdi);

error1:

error0:
        return rc;
}

void sivfs_kill_sb(struct super_block *sb)
{
        //yuck, the pairing between fill_sb and kill_sb is poor and kill_sb
        //is called even on a failed fill_sb. Hack to fix this:
        if (!sivfs_shared_state_inited){
                goto unfilled;
        }

        //Invariants
        struct sivfs_shared_state* sstate = sivfs_sb_to_shared_state(sb);

#if 0
        //sivfs_state should be automatically freed, but this is an extra check
        //Using for_each_safe so we can free elements as we go
        struct sivfs_state *state, *tmp;
        list_for_each_entry_safe(state, tmp, &sstate->mmap_list, mmap_item){
                destroy_sivfs_state(state);
                kfree(state);
        }
#endif

        //TODO want one of these per mount
        bdi_destroy(&sivfs_bdi);

        sivfs_destroy_shared_state(sstate);

unfilled:
        kill_litter_super(sb);

        dout("sivfs super unmounted");
        //Emit this as a PRINTK so it looks visually distinct
        printk(KERN_INFO "sivfs super unmounted");
}

struct dentry *sivfs_mount(struct file_system_type *fs_type,
        int flags, const char *dev_name, void *data)
{
        return mount_nodev(fs_type, flags, data, sivfs_fill_super);
}
