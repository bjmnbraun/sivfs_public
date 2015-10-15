#ifndef SIVFS_ALLOCATION_H
#define SIVFS_ALLOCATION_H

#include <linux/slab.h>

#include "../sivfs_radix_tree.h"
#include "../sivfs_types.h"

struct sivfs_state;

//the "alloc" directory which assists userspace with allocating
//space within files is an optional module, via this macro:
#define HAS_SIVFS_ALLOC

#define SIVFS_CHUNK_BOUNDARY_NULL ((size_t)-1)

#define SIVFS_CHUNK_ALLOCATED (1UL << 63)

//Set on a chunk that has been freed this transaction but has not yet been
//committed. Used to avoid coalescing into a tentative free.
#define SIVFS_CHUNK_FREEING (1UL << 62)

#define SIVFS_CHUNK_ALLOC_FLAGS (SIVFS_CHUNK_ALLOCATED | SIVFS_CHUNK_FREEING)

//Forward declaration
struct sivfs_allocator_info;

//zero initialization is fine
struct sivfs_alloc_chunk {
        //The last timestamp at which the chunk was published
        //as available. Basically we use this to defer visibility
        //of frees. SIVFS_CHUNK_ALLOCATED is OR'ed on if allocated or 
        //locked for allocation.
        sivfs_ts last_freed_ts;
        size_t start;
        size_t size;
        //Listnode used to hold chunk in one of the best-fit lists
        //Also used to hold chunk in threadlocal txn allocations list
        struct list_head lh_bestfit;
        //Listnode used to hold chunk in threadlocal txn freed list
        //This needs to be a different list node since a chunk can be 
        //allocated and later freed in the same txn
        struct list_head lh_to_free;
        //Slightly wasteful, but we have a pointer up to the containing
        //allocator info
        struct sivfs_allocator_info* ainfo;
};

HEADER_INLINE bool sivfs_alloc_chunk_is_free(
        struct sivfs_alloc_chunk* chunk
){
        return 0 == (chunk->last_freed_ts & SIVFS_CHUNK_ALLOCATED);
}

//Guards against 
HEADER_INLINE bool sivfs_alloc_chunk_is_free_visible(
        struct sivfs_alloc_chunk* chunk,
        sivfs_ts test_ts
){
        return
                sivfs_alloc_chunk_is_free(chunk) &&
                ((chunk->last_freed_ts & ~SIVFS_CHUNK_ALLOC_FLAGS) <= test_ts)
        ;
}

HEADER_INLINE int sivfs_new_alloc_chunk(
        struct sivfs_alloc_chunk** out,
        struct sivfs_allocator_info* ainfo
){
        struct sivfs_alloc_chunk* toRet = kzalloc(sizeof(**out),GFP_KERNEL);
        if (!toRet){
                return -ENOMEM;
        }
        toRet->ainfo = ainfo;
        *out = toRet;
        return 0;
}

HEADER_INLINE void sivfs_put_alloc_chunk(
        struct sivfs_alloc_chunk* chunk
){
        kfree(chunk);
}

//Allocator metadata for a particular page of a file
//zero initialization is fine
struct sivfs_allocator_pginfo {
        //Offset of the chunk boundary in this page, if any
        //(if none, use SIVFS_CHUNK_BOUNDARY_NULL)
        //Note - to keep things consistent we have ends_chunk and starts_chunk
        //== NULL if the boundary is NULL.
        size_t boundary_offset;
        //The boundary ends one chunk, possibly
        struct sivfs_alloc_chunk* ends_chunk;
        //The boundary starts a new chunk, possibly
        struct sivfs_alloc_chunk* starts_chunk;
};

HEADER_INLINE int sivfs_new_allocator_pginfo(
        struct sivfs_allocator_pginfo** out
){
        struct sivfs_allocator_pginfo* toRet = kzalloc(sizeof(**out),GFP_KERNEL);
        if (!toRet){
                return -ENOMEM;
        }
        *out = toRet;
        return 0;
}

HEADER_INLINE void sivfs_put_allocator_pginfo(
        struct sivfs_allocator_pginfo* pginfo
){
        kfree(pginfo);
}

//Allocator metadata for a particular file
struct sivfs_allocator_info {
        //TODO multiple best-fit lists by size category
        struct list_head bestfit_list;
        //Each chunk is started on exactly one pginfo
        struct radix_tree_root pginfo_tree;
        //Lock protects the above fields
        spinlock_t alloc_info_lock;
};

HEADER_INLINE void sivfs_init_allocator_info (
        struct sivfs_allocator_info* info
){
        spin_lock_init(&info->alloc_info_lock);
        INIT_RADIX_TREE(&info->pginfo_tree, GFP_KERNEL);
        INIT_LIST_HEAD(&info->bestfit_list);
};

HEADER_INLINE void sivfs_destroy_allocator_info(
        struct sivfs_allocator_info* info
){
        //Some checks.
        if (spin_is_locked(&info->alloc_info_lock)){
                dout(
                        "ERROR: "
                        "Destroying allocator info when locked! "
                        "Skipping destruction."
                );
                return;
        }

        {
                void** slot;
                struct radix_tree_iter iter;
                unsigned long indices[16];
                int i, nr;
                unsigned long index;
                //Acc to gmap_radix_tree_free, the stuff below should work.
                do {
                        nr = 0;
                        radix_tree_for_each_slot(
                                slot,
                                &info->pginfo_tree,
                                &iter,
                                0
                        ){
                                struct sivfs_allocator_pginfo* pginfo = *slot;

                                //Free any chunk this pginfo starts
                                struct sivfs_alloc_chunk* st_chunk =
                                        pginfo->starts_chunk
                                ;
                                if (st_chunk){
                                        sivfs_put_alloc_chunk(st_chunk);
                                }

                                sivfs_put_allocator_pginfo(pginfo);

                                indices[nr] = iter.index;
                                if (++nr == 16)
                                        break;
                        }
                        for(i = 0; i < nr; i++){
                                index = indices[i];
                                radix_tree_delete(
                                        &info->pginfo_tree,
                                        index
                                );
                        }
                } while (nr > 0);
        }

        if (!sivfs_radix_tree_empty(&info->pginfo_tree)){
                dout("ERROR: Alloc info still had pginfos!");
        }
}

HEADER_INLINE int sivfs_new_allocator_info (
        struct sivfs_allocator_info** info_out
){
        int rc = 0;
        struct sivfs_allocator_info* info;
        info = kzalloc(sizeof(*info), GFP_KERNEL);
        if (!info){
                rc = -ENOMEM;
                goto error0;
        }
        sivfs_init_allocator_info(info);

        *info_out = info;

error0:
        return rc;
};

HEADER_INLINE void sivfs_put_allocator_info(
        struct sivfs_allocator_info* info
){
        if (unlikely(ZERO_OR_NULL_PTR(info))){
                return;
        }

        sivfs_destroy_allocator_info(info);
        kfree(info);
}

int sivfs_alloc_get_pginfo(
        struct sivfs_allocator_pginfo** pginfo_out,
        struct sivfs_allocator_info* ainfo,
        size_t offset
);


int sivfs_alloc_info_recover(
        struct sivfs_allocator_info* info,
        void* recovery_info
);

//Allocates a block of a file, to support userspace dynamic
//allocators. Since this is likely to be a slow operation, userspace allocators
//should allocate large blocks and subdivide them into chunks as necessary
//which are used to serve dynamic allocation requests.
//
//To prevent userspace mucking around with the blocklist,
//block metadata is stored in an OS-side transient
//datastructure and also persistently in the metadata of the file.
//The persistence mechanism is: we add special write entries to commit log entries that
//include changes to this block metadata and the checkpointing mechanism
//applies these metadata changes to the metadata associated with checkpoint
//pages. On recovery, we can quickly do an OS-side scan to recover the
//transient OS-side datastructure.
//
//The filesystem also has some support for sharing lists of free chunks between
//users of the file, but that is seperate.
int sivfs_alloc_block(
        size_t* alloc_out,
        size_t* alloc_size_out,
        struct sivfs_state* state,
        struct sivfs_allocator_info* alloc_info,
        size_t size_requested
);

int sivfs_free_block(
        struct sivfs_state* state,
        struct sivfs_allocator_info* alloc_info,
        size_t block_offset
);

//Called after commit to update the transient datastructures to represent
//allocated / freed blocks during the transaction
int sivfs_allocation_on_commit(
        struct sivfs_state* state,
        bool aborted,
        sivfs_ts commit_ts
);

#endif
