#ifndef SIVFS_FILE_INFO_H
#define SIVFS_FILE_INFO_H

#include <linux/file.h>
#include <linux/fs.h>

struct sivfs_state;

//
//Information associated with an open sivfs file
//Basically stores a pointer to the state, and fast path pointers for writing
//to the underlying file
//
//Looks like struct files are not copied on forks.
struct sivfs_file_info {
        //The struct file we are associated with
        struct file* file;
        //The sivfs_state we are a part of
        struct sivfs_state* state;
        //struct file* that can be used to directly access underlying file
        //If this pointer is set, free it with fput on destruction
        //Deprecated - sivfs does not use this.
        struct file* direct_access_file;
};

HEADER_INLINE struct sivfs_file_info* sivfs_file_to_finfo(
        struct file* file
){
        return file->private_data;
}

HEADER_INLINE struct sivfs_file_info* sivfs_vma_to_finfo(
        struct vm_area_struct* vma
){
        return sivfs_file_to_finfo(vma->vm_file);
}

HEADER_INLINE struct sivfs_state* sivfs_file_to_state(
        struct file* file
){
        return sivfs_file_to_finfo(file)->state;
}

HEADER_INLINE int sivfs_new_finfo(struct sivfs_file_info** new_finfo){
        int rc = 0;

        struct sivfs_file_info* finfo;
        finfo = kzalloc(sizeof(*finfo), GFP_KERNEL);

        if (!finfo){
                rc = -ENOMEM;
                goto error0;
        }

out:
        *new_finfo = finfo;
        return rc;

error0:
        return rc;
}

//Used for freeing the result of new_file_info
HEADER_INLINE void sivfs_put_finfo(struct sivfs_file_info* finfo){
        //fput direct_access_file, if we have one
        //we create it lazily to allow read-only opens
        //to skip this
        struct file* file = finfo->direct_access_file;
        if (file)
                fput(file);
        kfree(finfo);
}

#endif
