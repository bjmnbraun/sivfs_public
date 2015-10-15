#ifndef _SIVFS_H
#define _SIVFS_H

//Surprisingly enough, these have equivalent forms in userland
//Needed for IOWR
#include <linux/types.h>
#include <linux/major.h>
#include <linux/string.h>

#include "sivfs_writeset.h"
#include "sivfs_stats.h"

//Args to ioctls
struct sivfs_args_t {
        bool aborted;
        struct sivfs_writeset ws;
        struct sivfs_stats* stats;
        void* target_address;
        size_t buf_size;
        char* buf;
        //forces the commit to abort, useful for removing staged writes
        bool abort;
        //Update to the latest checkpoint, and check it out for direct writing.
        //TODO we need to have some robust concurrency control here to prevent
        //multiple direct writers at a time / direct writers concurrent with
        //normal writers. 
        bool wants_direct_access;
};

struct sivfs_allocator_args_t {
        //outputs
        void* mmap_start;
        size_t mmap_size;
        void* alloc_ret;
        size_t alloc_size;
        //inputs
        void* zone;
        size_t block_alloc;
};

struct sivfs_page_info {
        unsigned long address;
        unsigned long pte_path [4];
        unsigned long swp_ent;
        void* page;
        bool is_dirty;
        //The next two bools should always agree
        bool is_page_file_backed;
        bool is_vma_file_backed;
        //True if the page is a checkpoint page
        bool is_checkpoint_page;
        //True if a checkpoint is mapped into the containing vma
        bool is_checkpoint_mapped;
        bool is_invalid_mapcount;
        bool is_page_unevictable;
        bool bdi_accounts_writeback;
        const char* pte_flags;
};


//No excuse for not properly sanitizing input inside ioctl handler, by the way
HEADER_INLINE void sivfs_init_args(struct sivfs_args_t* args){
        memset(args, 0, sizeof(*args));
}

/*
 * IOCTL interface
 * Linus reccomends the convention of passing the major version number
 * as the first argument to these, since we have a misc device this is
 * MISC_MAJOR
 */
#define SIVFS_TXN_COMMIT_UPDATE _IOWR(MISC_MAJOR, 0x01, struct sivfs_args_t)
#define SIVFS_TXN_PAUSE         _IO(MISC_MAJOR, 0x02)
#define SIVFS_GET_STATS	        _IOWR(MISC_MAJOR, 0x03, struct sivfs_args_t)
#define SIVFS_GET_PAGE_INFO     _IOWR(MISC_MAJOR, 0x04, struct sivfs_args_t)

#define SIVFS_ALLOC_BLOCK       _IOWR(MISC_MAJOR, 0x05, struct sivfs_allocator_args_t)
#define SIVFS_FREE_BLOCK        _IOWR(MISC_MAJOR, 0x06, struct sivfs_allocator_args_t)
#define SIVFS_ALLOC_FREE_LIST   _IOWR(MISC_MAJOR, 0x07, struct sivfs_allocator_args_t)
#define SIVFS_FREE_FREE_LIST    _IOWR(MISC_MAJOR, 0x08, struct sivfs_allocator_args_t)

#ifdef __KERNEL__

/* inode.c - operations on inodes */
extern const struct file_operations sivfs_dir_operations;
extern const struct inode_operations sivfs_dir_inode_operations;
extern const struct file_operations sivfs_file_operations;
extern const struct inode_operations sivfs_file_inode_operations;
extern const struct address_space_operations sivfs_anon_addr_operations;
extern const struct file_operations sivfs_anon_file_operations;
extern const struct inode_operations sivfs_anon_file_inode_operations;
extern const struct file_operations sivfs_checkpoint_file_operations;
extern const struct inode_operations sivfs_checkpoint_inode_operations;
extern const struct file_operations sivfs_commit_file_operations;
extern const struct inode_operations sivfs_commit_inode_operations;

extern const struct vm_operations_struct generic_file_vm_ops;
extern const struct address_space_operations sivfs_aops;
extern const struct mmu_notifier_ops sivfs_mmuops;

struct inode *sivfs_get_inode(
        struct super_block *sb,
        const struct inode *dir,
        umode_t mode,
        dev_t dev
);
int sivfs_drop_inode(struct inode *inode);
void sivfs_evict_inode(struct inode *inode);

struct sivfs_state;
int sivfs_create_anon_inode(
        struct inode** out_inode,
        struct inode* file_inode,
        struct sivfs_state* state
);
int sivfs_create_anon_dentry(
        struct dentry** anon_dentry,
        struct inode* anon_inode
);
int sivfs_create_checkpoint_inode(
        struct inode** out_inode,
        struct super_block* sb
);

struct inode *sivfs_d_select_inode(struct dentry *dentry, unsigned file_flags);
struct dentry *sivfs_d_real(
        struct dentry *dentry,
        const struct inode *inode,
        unsigned file_flags
);

//Takes a struct file* of an open anonymous file, and
//open the underlying file for direct access into out_file
int sivfs_open_direct_access(struct file** out_file, struct file* file);

//Unmaps a checkpoint mapped into vma
void sivfs_unmap_checkpoint(struct vm_area_struct* vma);

/* module.c - module handling */
//Gets page info for a virtual address
int sivfs_get_page_info_current(
        struct sivfs_page_info* info,
        unsigned long addr
);

/* super.c - mount handling */
extern const struct super_operations sivfs_ops;

struct dentry *sivfs_mount(
        struct file_system_type *fs_type,
        int flags,
        const char *dev_name,
        void *data
);

int sivfs_fill_super(struct super_block *sb, void *data, int silent);
void sivfs_kill_sb(struct super_block *sb);

HEADER_INLINE void sivfs_destroy_args(struct sivfs_args_t* argstruct){
        //kfree properly handles null inputs
        kfree(argstruct->ws.entries);
}

HEADER_INLINE int sivfs_page_info_to_string(
        char* buf,
        size_t buf_size,
        struct sivfs_page_info* info
){
        return snprintf(
                buf,
                buf_size,

                ">Pginfo\t%p\t%016lx\t%016lx\t%016lx\t%016lx"
                "\t%s%s\t%s\t%s\t%s\t%s\t%s\t%s"
                "\tpage\t%p\tswap\t%016lx\t%s"
                ,

                (void*)info->address,
                info->pte_path[0],
                info->pte_path[1],
                info->pte_path[2],
                info->pte_path[3],

                info->is_dirty?"D":"0",
                info->is_vma_file_backed?"FB":"0",
                info->is_page_file_backed?"fb":"0",
                info->is_checkpoint_page?"CP":"0",
                info->is_checkpoint_mapped?"CCO":"0",
                info->is_invalid_mapcount?"MC":"0",
                info->bdi_accounts_writeback? "dWB":"0",
                info->is_page_unevictable?"UE":"0",

                info->page,
                info->swp_ent,
                info->pte_flags
        );
}


#else

#include <assert.h>

//Userspace versions of functions

//user-space version of this function differs from kernel veresion
HEADER_INLINE void sivfs_destroy_args(struct sivfs_args_t* argstruct){
        //free handles null inputs
        free(argstruct->ws.entries);
}

//XXX
#include <iostream>

//Friendly way to add an element to writeset and also write to thread's
//workingset
HEADER_INLINE int sivfs_write_ws(struct sivfs_writeset* ws, volatile void* address, sivfs_word_t value){
        if (((unsigned long)address) % sizeof(sivfs_word_t)){
                return -EINVAL;
        }

        ((volatile sivfs_word_t*)address)[0] = value;

        DEFINE_WRITESET_ENTRY(entry, address, value);

        return sivfs_add_to_writeset(
                ws,
                &entry,
                sizeof(entry)
        );
}

HEADER_INLINE int sivfs_write_wsp(struct sivfs_writeset* ws, volatile void* address, void* ptr){
        return sivfs_write_ws(ws, address, (sivfs_word_t)ptr);
}

#endif

#endif
