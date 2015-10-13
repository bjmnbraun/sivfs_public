#ifndef _SIVFS_H
#define _SIVFS_H

#include <linux/types.h>
#include <linux/major.h>

/* SIVFS definitions
 *
 * Copyright (C) 2005 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

extern const struct inode_operations sivfs_file_inode_operations;

struct inode *sivfs_get_inode(struct super_block *sb, const struct inode *dir,
	umode_t mode, dev_t dev);
int sivfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
extern struct dentry *sivfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data);

static inline int
sivfs_nommu_expand_for_mapping(struct inode *inode, size_t newsize)
{
	return 0;
}

extern const struct file_operations sivfs_file_operations;
extern const struct vm_operations_struct generic_file_vm_ops;
extern int __init init_sivfs_fs(void);

int sivfs_fill_super(struct super_block *sb, void *data, int silent);


typedef struct {
  int result;
  unsigned long long int nr_flushes;
} sivfs_args_t;

#ifndef __KERNEL__
struct sivfs_stats {
    unsigned long long int nivcsw;
    unsigned long long int nvcsw;
    unsigned long long int ninterrupts;
    unsigned long long int nticks;
};
#endif

/*
 * IOCTL interface
 * Linus reccomends the convention of passing the major version number
 * as the first argument to these, since we have a misc device this is
 * MISC_MAJOR
 * Type is incorrect but linux apparently doesn't check this?
 */
#define SIVFS_ENABLE	_IOWR(MISC_MAJOR, 0x01, sivfs_args_t)
#define SIVFS_DISABLE	_IOWR(MISC_MAJOR, 0x02, sivfs_args_t)
#define SIVFS_NR_FLUSHES _IOWR(MISC_MAJOR, 0x03, sivfs_args_t)

#endif
