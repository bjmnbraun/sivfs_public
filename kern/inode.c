/*
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
#include "sivfs_state.h"
#include "sivfs_shared_state.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Snapshot isolation virtual file system");

#define SIVFS_DEFAULT_MODE	0755
#define SIVFS_MAGIC 0x203984

static const struct super_operations sivfs_ops;
static const struct inode_operations sivfs_dir_inode_operations;
static const struct file_operations sivfs_dir_operations;

int __set_page_dirty_no_writeback_(struct page* page){
  //Copied from __set_page_dirty_no_writeback
  if (!PageDirty(page))
    return !TestSetPageDirty(page);
  return 0;
}

static const struct address_space_operations sivfs_aops = {
	.readpage	= simple_readpage,
	.write_begin	= simple_write_begin,
	.write_end	= simple_write_end,
	.set_page_dirty	= __set_page_dirty_no_writeback_,
};

//Creates an inode with owner dir under dev dev
//but does NOT link the inode into the directory tree
//so this can be used to create invisible inodes
struct inode *sivfs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
                dout("Created inode %p with mode %o", inode, mode);

		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		inode->i_mapping->a_ops = &sivfs_aops;
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
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

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			break;
		}
	}
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
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	}
	return error;
}

//Make a directory inode and link into directory tree under dev 0
static int sivfs_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode)
{
	int retval = sivfs_mknod(dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

//Make a inode and link into directory tree under dev 0
int sivfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
      return sivfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

//Create a symlink and link into directory tree under dev 0
static int sivfs_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
{
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

static const struct inode_operations sivfs_dir_inode_operations = {
	.create		= sivfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= sivfs_symlink,
	.mkdir		= sivfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= sivfs_mknod,
	.rename		= simple_rename,
};

static const struct file_operations sivfs_dir_operations = {
	.open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.iterate	= dcache_readdir,
	.fsync		= noop_fsync,
};

static const struct super_operations sivfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= generic_show_options,
};

struct sivfs_mount_opts {
	umode_t mode;
};

enum {
	Opt_mode,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_mode, "mode=%o"},
	{Opt_err, NULL}
};

struct sivfs_fs_info {
	struct sivfs_mount_opts mount_opts;
};

static int sivfs_parse_options(char *data, struct sivfs_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	char *p;

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

	return 0;
}

int sivfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct sivfs_fs_info *fsi;
	struct inode *inode;
	int err;

	save_mount_options(sb, data);

	fsi = kzalloc(sizeof(struct sivfs_fs_info), GFP_KERNEL);
	sb->s_fs_info = fsi;
	if (!fsi)
		return -ENOMEM;

	err = sivfs_parse_options(data, &fsi->mount_opts);
	if (err)
		return err;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_CACHE_SIZE;
	sb->s_blocksize_bits	= PAGE_CACHE_SHIFT;
	sb->s_magic		= SIVFS_MAGIC;
	sb->s_op		= &sivfs_ops;
	sb->s_time_gran		= 1;

	inode = sivfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

        dout("sivfs successfully mounted");

	return 0;
}

struct dentry *sivfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, sivfs_fill_super);
}

static void sivfs_kill_sb(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}

static struct file_system_type sivfs_fs_type = {
        .owner          = THIS_MODULE,
	.name		= "sivfs",
	.mount		= sivfs_mount,
	.kill_sb	= sivfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

//Device operations on /dev/sivfs
struct file_operations sivfs_chardev_ops = {
	.owner		= THIS_MODULE,
//	.unlocked_ioctl	= timing_defense_dev_ioctl,
#ifdef CONFIG_COMPAT
//	.compat_ioctl	= timing_defense_dev_ioctl,
#endif
//	.llseek		= noop_llseek,
//      .mmap           = my_mmap,
};

static struct miscdevice sivfs_dev = {
	MISC_DYNAMIC_MINOR,
	"sivfs",
	&sivfs_chardev_ops,
};

int __init init_sivfs_fs(void)
{
	int ret;

        //Initialize members here
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        ret = init_sivfs_shared_state(sstate);
        if (ret < 0)
          goto error_0;

        //Now publicize the filesystem
        ret = misc_register(&sivfs_dev);
        if (ret < 0)
          goto error_0;

        ret = register_filesystem(&sivfs_fs_type);
        if (ret < 0)
          goto error_1;

        //Emit this as a PRINTK so it looks visually distinct
        printk(KERN_INFO "sivfs successfully inited");
        dout("sivfs successfully inited");
        return 0;
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
        //TODO currently assuming that here, there are no open sivfs mounts
        //I'm pretty sure this is the case but double check exactly what the
        //mechanism is that ensures this
        //NOT ACQUIRING ANY LOCKS BELOW (!!!)

        misc_deregister(&sivfs_dev);
	unregister_filesystem(&sivfs_fs_type);

        //Clean up
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        //Using for_each_safe so we can free elements as we go
        struct sivfs_state *state, *tmp;
        list_for_each_entry_safe(state, tmp, &sstate->mmap_list, mmap_item){
                destroy_sivfs_state(state);
                kfree(state);
        }
        //Invariants
        dout("sivfs successfully uninited");
        //Emit this as a PRINTK so it looks visually distinct
        printk(KERN_INFO "sivfs successfully uninited");
}

module_init(init_sivfs_fs);
module_exit(exit_sivfs_fs);
