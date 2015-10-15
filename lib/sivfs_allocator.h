#ifndef SIVFS_ALLOCATOR_H
#define SIVFS_ALLOCATOR_H

//Needed for offsetof
#include "stddef.h"
#include "assert.h"
#include "sivfs.h"
#include "sivfs_allocator_stats.h"
#include "sivfs_allocator_common.h"
#include "sivfs_allocator_bins.h"
#include "sivfs_allocator_config.h"
#include "sivfs_stack.h"

//For ioctl
#include <fcntl.h>
#include <sys/ioctl.h>

#define SIVFS_ALLOC 1

//TODO make allocation regions point to themselves in a position-independent
//way, i.e. with relative offsets insead of pointers.

//TODO everything that uses object size should be changed to use allocated_size
//(i.e. including the 64-bit allocated size header). This way we never have to
//add or subtract the header size internally in our methods, rather this is
//done at the top level, which is cleaner.

//TODO writeset should be the first argument of more methods, since it is
//modified

//list head struct
struct sivfs_allocator_lh {
        struct sivfs_allocator_lh* prev;
        struct sivfs_allocator_lh* next;
};

//Does not include "head" during iteration.
#define sivfs_alloc_lh_for_each(pos, head) \
        for (pos = (head)->next; pos != (head); pos = pos->next)

//Does not count head as an element
HEADER_INLINE bool sivfs_alloc_list_empty(struct sivfs_allocator_lh* head){
        return head->next == head;
}

HEADER_INLINE struct sivfs_allocator_lh* sivfs_alloc_lh_advance_incl(
        struct sivfs_allocator_lh* iter,
        struct sivfs_allocator_lh* head
){
        if (iter == head){
                return NULL;
        }
        return iter->next;
}
//Includes head during iteration. Head is the last element iterated over.
#define sivfs_alloc_lh_for_each_incl(pos, head) \
        for (pos = (head)->next; pos; pos = sivfs_alloc_lh_advance_incl(pos,(head)))

//Only used for initializing the head element, by making it a properly circular
//list. For list nodes intended to be added to some existing list, just
//initialize to 0s.
#define sivfs_init_alloc_lh_head(head) {(head)->next = (head)->prev = (head);}

HEADER_INLINE void sivfs_alloc_lh_add(
        struct sivfs_allocator_lh* list,
        struct sivfs_allocator_lh* lh
){
        //We will insert lh so it comes before target
        struct sivfs_allocator_lh* target = list->next;
        //Equivalently, target_prev == list
        struct sivfs_allocator_lh* target_prev = target->prev;

        lh->next = target;
        lh->prev = target_prev;
        target_prev->next = lh;
        target->prev = lh;
}

HEADER_INLINE int sivfs_alloc_lh_add_ws(
        struct sivfs_writeset* ws,
        struct sivfs_allocator_lh* list,
        struct sivfs_allocator_lh* lh
){
        int rc = 0;

        //We will insert lh so it comes before target
        struct sivfs_allocator_lh* target = list->next;
        //Equivalently, target_prev == list
        struct sivfs_allocator_lh* target_prev = target->prev;

        //Any error below is fatal...
        rc = sivfs_write_wsp(ws, &lh->next, target);
        if (rc) goto error0;
        rc = sivfs_write_wsp(ws, &lh->prev, target_prev);
        if (rc) goto error0;
        rc = sivfs_write_wsp(ws, &target_prev->next, lh);
        if (rc) goto error0;
        rc = sivfs_write_wsp(ws, &target->prev, lh);
        if (rc) goto error0;

error0:
        return rc;
}

HEADER_INLINE void sivfs_alloc_lh_remove(struct sivfs_allocator_lh* lh){
        struct sivfs_allocator_lh* prev = lh->prev;
        struct sivfs_allocator_lh* next = lh->next;

        if (lh == prev || lh == next){
                //This means we tried to remove a list header, which is not a
                //real element of the list
                uout("ERROR - attempt to remove list header. About to self destruct..");
                return;
        }
        if (!prev || !next){
                //lh was not in a list
                uout("ERROR - attempt to remove an element not in a list.");
                return;
        }

        lh->prev = NULL;
        lh->next = NULL;
        prev->next = next;
        next->prev = prev;
}

HEADER_INLINE int sivfs_alloc_lh_remove_ws(
        struct sivfs_writeset* ws,
        struct sivfs_allocator_lh* lh
){
        int rc = 0;
        struct sivfs_allocator_lh* prev = lh->prev;
        struct sivfs_allocator_lh* next = lh->next;

        if (lh == prev || lh == next){
                //This means we tried to remove a list header, which is not a
                //real element of the list
                uout("ERROR - attempt to remove list header. About to self destruct..");
                rc = -EINVAL;
                goto error0;
        }
        if (!prev || !next){
                //lh was not in a list
                uout("ERROR - attempt to remove an element not in a list.");
                rc = -EINVAL;
                goto error0;
        }

        //The next two settings to NULL are optional.
        rc = sivfs_write_wsp(ws, &lh->prev, NULL);
        if (rc) goto error0;
        rc = sivfs_write_wsp(ws, &lh->next, NULL);
        if (rc) goto error0;

        rc = sivfs_write_wsp(ws, &prev->next, next);
        if (rc) goto error0;
        rc = sivfs_write_wsp(ws, &next->prev, prev);
        if (rc) goto error0;

error0:
        return rc;
}

//bits 63-63 = 1 bit
const size_t SIVFS_BIG_ALLOCATION_FLAG = (1UL<<63UL);

//bits 62-62 = 1 bit
const size_t SIVFS_LIST_HEAD_FLAG = (1UL<<62UL);

//bits 46-61 = 16 bits
const size_t SIVFS_FREED_MAGIC = (0xdeadUL<<46UL);
const size_t SIVFS_FREED_MAGIC_MASK = (0xffffUL << 46UL);

const size_t SIVFS_ALLOCATION_FLAGS = 
        SIVFS_BIG_ALLOCATION_FLAG
        | SIVFS_LIST_HEAD_FLAG
        | SIVFS_FREED_MAGIC_MASK
;

//Also store the object index-in-slab in the slab object so we can quickly
//look up the containing slab

//bits 30-45 = 16 bits
const size_t SIVFS_SLAB_INDEX_MASK = (0xffffUL << 30UL);
const size_t SIVFS_SLAB_INDEX_SHIFT = 30UL;

//But this imposes a restriction on # slab objects per slab, so enforce that:
const size_t SIVFS_MAX_SLAB_ELTS = (1UL<<16UL)-1;

const size_t SIVFS_ALLOCATOR_SLABSIZE = 4096*4;

//Represents a slab object, but also type-punned to represent slabs
//When allocated, only the first word is valid. Includes the size of the
//header, i.e. the size describes the entire region.
//Magic bits are ORed onto the allocated_size as a check for
//double-free.
//When free, lh is also valid and points to the next / last entry in some free
//list. 
//allocated_size also has a SIVFS_LIST_HEAD_FLAG set if the free element is
//the "head" of a free list, which should be allocated last. The head element
//of a list acts as a canonical element for locking the entire free list.
//
//For slab objects, further bits of allocated_size are used to encode the 
//index in the slab, via SIVFS_SLAB_INDEX_MASK
struct sivfs_allocated_region {
        size_t allocated_size;
        struct sivfs_allocator_lh lh;
};

//Just the size of the allocated_size header, needed to perform freeing
#define SIVFS_ALLOC_REGION_HEADER_SIZE sizeof(size_t)

//For blocks, the first and last word hold the size of the block
//also the second to last word is a tag for the slab
//Slabs are allocated within blocks, i.e. this header/footer is added in
//addition to the allocated_region header for the slab elements. They act
//as protectors against coalescing neighboring big blocks into the slab.
//
//We also store the tag for the block in the second to last word
#define SIVFS_BLOCK_HEADER_SIZE sizeof(size_t)
#define SIVFS_BLOCK_FOOTER_SIZE (sizeof(uint64_t) + sizeof(size_t))

//Useful macro
#define DIV_ROUND(x, len) (((x) + (len) - 1) / (len))
#define ROUND_UP(x, align) DIV_ROUND(x, align) * (align)

//Marks region as allocated or freed, depending on alloc
HEADER_INLINE int sivfs_allocated_region_set_flags(
        struct sivfs_writeset* ws,
        struct sivfs_allocated_region* region,
        size_t flags
){
        if (flags & ~SIVFS_ALLOCATION_FLAGS){
                dout("Here");
                return -EINVAL;
        }

        return sivfs_write_ws(
                ws,
                &region->allocated_size,
                (region->allocated_size & ~SIVFS_ALLOCATION_FLAGS) | flags
        );
}

HEADER_INLINE size_t sivfs_allocated_region_size(
        struct sivfs_allocated_region* region
){
        if (region->allocated_size & SIVFS_BIG_ALLOCATION_FLAG){
                return region->allocated_size & ~SIVFS_ALLOCATION_FLAGS;
        } else {
                return region->allocated_size & ~SIVFS_ALLOCATION_FLAGS & ~SIVFS_SLAB_INDEX_MASK;
        }
}

HEADER_INLINE size_t sivfs_allocated_region_flags(
        struct sivfs_allocated_region* region
){
        return region->allocated_size & SIVFS_ALLOCATION_FLAGS;
}

//Only defined for slab objects
HEADER_INLINE size_t sivfs_allocated_region_slab_index(
        struct sivfs_allocated_region* region
){
        return
                (region->allocated_size & SIVFS_SLAB_INDEX_MASK) >>
                SIVFS_SLAB_INDEX_SHIFT
        ;
}


HEADER_INLINE void* sivfs_alloc_region_to_block_head(
        struct sivfs_allocated_region* region
){
        if (region->allocated_size & SIVFS_BIG_ALLOCATION_FLAG){
                return region;
        }

        //Otherwise it's a slab object

        size_t slab_index = sivfs_allocated_region_slab_index(region);
        size_t slab_elt_size = sivfs_allocated_region_size(region);
        region = (struct sivfs_allocated_region*)((char*)region - slab_index * slab_elt_size);

        //Sanity test
        if (sivfs_allocated_region_slab_index(region) != 0){
                uout("ERROR - wrong slab index / not a slab elt");
                return NULL;
        }

        void* slab_head = ((char*)region) - SIVFS_BLOCK_HEADER_SIZE;

        //Do some checking
        size_t block_size = ((size_t*)slab_head)[0];
        if (block_size == 0){
                uout("ERROR - wrong slab index / not a slab elt");
                return NULL;
        }

        void* block_footer_ptr = 
                ((char*)slab_head) + block_size -
                SIVFS_BLOCK_FOOTER_SIZE
        ;
        size_t _block_size = 
                ((size_t*)((char*)block_footer_ptr + sizeof(uint64_t)))[0]
        ;
        if (_block_size != block_size){
                uout("ERROR - block corruption detected");
                return NULL;
        }

        return slab_head;
}

HEADER_INLINE uint64_t sivfs_block_tag(void* block_head){
        //Two kinds of blocks headers - non-slab have a single bit 
        //we need to chop off
        size_t block_size = 
                ((size_t*)block_head)[0] & ~SIVFS_BIG_ALLOCATION_FLAG
        ;
        //Do some checking
        if (block_size == 0){
                //Assertion error
                assert(0);
                return 0;
        }

        void* block_footer_ptr = 
                ((char*)block_head) + block_size -
                SIVFS_BLOCK_FOOTER_SIZE
        ;
        size_t _block_size = 
                ((size_t*)((char*)block_footer_ptr + sizeof(uint64_t)))[0]
        ;
        if (_block_size != block_size){
                uout("ERROR - block corruption detected");
                assert(0);
                return 0;
        }

        return ((uint64_t*)block_footer_ptr)[0];
        
}

HEADER_INLINE struct sivfs_allocated_region* sivfs_alloc_ret_to_region(void* ptr){
        return (struct sivfs_allocated_region*)(
                (char*)ptr - SIVFS_ALLOC_REGION_HEADER_SIZE
        );
}

HEADER_INLINE void* sivfs_allocated_region_to_alloc_ret(struct sivfs_allocated_region* region){
        return (char*)region + SIVFS_ALLOC_REGION_HEADER_SIZE;
}


HEADER_INLINE struct sivfs_allocated_region* sivfs_allocator_lh_to_allocated_region(
        struct sivfs_allocator_lh* lh
){
        return (struct sivfs_allocated_region*)(
                (char*)lh - offsetof(struct sivfs_allocated_region, lh)
        );
}

//Userside metadata on a per-vma basis
struct sivfs_allocator_mmap_state {
        //Freelists binned by size
        struct sivfs_allocator_bins free_lists;
        //The domain of this state, by vma start and size 
        void* start;
        uint64_t tag;
        size_t size;
};

HEADER_INLINE void sivfs_init_allocator_mmap_state(
        struct sivfs_allocator_mmap_state* state
){
        memset(state, 0, sizeof(*state));
}

HEADER_INLINE struct sivfs_allocator_mmap_state* sivfs_new_allocator_mmap_state(void)
{
        struct sivfs_allocator_mmap_state* toRet;
        toRet = (struct sivfs_allocator_mmap_state*) malloc(sizeof(*toRet));
        if (!toRet){
                goto error0;
        }

        sivfs_init_allocator_mmap_state(toRet);

error0:
        return toRet;
}

HEADER_INLINE void sivfs_put_allocator_mmap_state(
        struct sivfs_allocator_mmap_state* mstate
){
        free(mstate);
}


struct sivfs_allocator_state {
        int sivfs_mod_fd;
        struct sivfs_allocator_args_t args;

        //Currently open vma with allocator state
        //TODO: keep this small by garbage collecting over time (get rid of unused free lists)
        //In that sense, this is really a "cache" of free lists with some fixed
        //capacity. _add_mmap is used to invalidate cache entries as needed
        //when adding new mmaps.
        struct sivfs_stack mmap_states;

        //On allocating or freeing we modify mmap_states' free lists, however
        //on abort we need to roll back the changes to how they were before.
        //These two stacks let us do that:
        struct sivfs_stack restore_old_free_list_ptrs; //type is free_list**
        struct sivfs_stack restore_old_free_list_values; //type is free_list*

        struct sivfs_allocator_stats stats;
};

HEADER_INLINE int sivfs_allocator_state_init(struct sivfs_allocator_state* state){
        int rc = 0;

        memset(state, 0, sizeof(*state));

        state->sivfs_mod_fd = open("/dev/sivfs", O_RDWR);
        if (state->sivfs_mod_fd == -1){
                rc = -EINVAL;
        }

        return rc;
}

extern thread_local struct sivfs_allocator_state* _sivfs_allocator_state;
HEADER_INLINE struct sivfs_allocator_state* get_sivfs_allocator_state(){
        return _sivfs_allocator_state;
}

//May return -EAGAIN which means that there was an error due to a conflicting
//concurrent free - which should cause the current transaction to abort/ retry.
HEADER_INLINE int sivfs_set_free_list(
        struct sivfs_allocator_lh** slot,
        struct sivfs_allocator_lh* list
){
        int rc = 0;

        struct sivfs_allocator_lh* old_list = *slot;
        
        struct sivfs_allocator_state* astate = get_sivfs_allocator_state();
        if (!astate){
                rc = -EINVAL;
                goto error0;
        }

        if (old_list){
                //TODO need to do an ioctl here to delete old_list from 
                //global list on commit
        }

        if (list){
                //TODO need to do an ioctl here to lock list. Release the lock
                //if this txn aborts.

                //The lock may fail due to a concurrent conflicting free
                //locking the same list. In that case, return -EAGAIN.
                bool could_lock = true;
                if (!could_lock){
                        rc = -EAGAIN;
                        goto error0;
                }
        }

        //Store the old value in restore_old_list_ptrs
        rc = sivfs_stack_push(&astate->restore_old_free_list_ptrs, slot);
        if (rc) goto error0;
        rc = sivfs_stack_push(&astate->restore_old_free_list_values, old_list);
        if (rc) {
                sivfs_stack_pop(&astate->restore_old_free_list_ptrs);
                goto error0;
        }

        //Do the assignment
        *slot = list;

error0:
        return rc;
};

HEADER_INLINE int sivfs_lib_allocation_on_commit(bool aborted){
        int rc = 0;

        struct sivfs_allocator_state* astate = get_sivfs_allocator_state();
        if (!astate){
                //Allocator not set up, so just return. Note that free and 
                //malloc both return error in this case so we know we haven't
                //used the allocation system.
                goto out;
        }

        if (aborted){
                //Go backwards over list and apply
                size_t i;
                if (astate->restore_old_free_list_ptrs.size){
                        for (i = astate->restore_old_free_list_ptrs.size - 1; ; ){
                                struct sivfs_allocator_lh** slot;
                                struct sivfs_allocator_lh* slot_value;

                                slot = (struct sivfs_allocator_lh**) astate->restore_old_free_list_ptrs.values[i];
                                slot_value = (struct sivfs_allocator_lh*) astate->restore_old_free_list_values.values[i];

                                *slot = slot_value;

                                //Advance
                                if (i == 0){
                                        break;
                                }
                                i--;
                        }
                }
        } else {
                //Forget the history
        }
        astate->restore_old_free_list_ptrs.size = 0;
        astate->restore_old_free_list_values.size = 0;

out:
error0:
        return rc;
}

HEADER_INLINE struct sivfs_allocator_mmap_state* sivfs_get_allocator_mmap_state(
        struct sivfs_allocator_state* state,
        void* zone,
        uint64_t tag
){
        int rc = 0;

        struct sivfs_allocator_mmap_state* toRet = NULL;

        size_t i;
        for(i = 0; i < state->mmap_states.size; i++){
                struct sivfs_allocator_mmap_state* mstate;
                mstate = (struct sivfs_allocator_mmap_state*)
                        state->mmap_states.values[i]
                ;
                void* vend = (char*)mstate->start + mstate->size;

                if (mstate->start <= zone && zone < vend && mstate->tag == tag){
                        //Found a match!
                        toRet = mstate;
                        break;
                }
        }

        return toRet;
}

HEADER_INLINE bool sivfs_mmap_state_overlaps(
        struct sivfs_allocator_mmap_state* state,
        void* start,
        uint64_t tag,
        size_t size
){
        void* end = (char*)start + size;
        void* vend = (char*)state->start + state->size;

        return state->tag == tag && state->start < end && vend >= start; 
}

HEADER_INLINE int sivfs_alloc_block(
        void** mmap_start,
        size_t* mmap_size,
        void** block_start,
        size_t* block_size,
        void* zone,
        size_t block_alloc
){
        int rc = 0;

        struct sivfs_allocator_state* state;
        struct sivfs_allocator_args_t* args;
        state = get_sivfs_allocator_state();
        if (!state){
                rc = -EINVAL;
                goto error0;
        }
        args = &state->args;
        args->zone = zone;
        args->block_alloc = block_alloc;
        rc = ioctl(state->sivfs_mod_fd, SIVFS_ALLOC_BLOCK, args);
        if (rc){
                uout("Here");
                goto error0;
        }

out:
        *mmap_start = args->mmap_start;
        *mmap_size = args->mmap_size;
        *block_start = args->alloc_ret;
        *block_size = args->alloc_size;

error0:
        return rc;
}

HEADER_INLINE int sivfs_free_block(
        void* block
){
        int rc = 0;

        struct sivfs_allocator_state* state;
        struct sivfs_allocator_args_t* args;
        state = get_sivfs_allocator_state();
        if (!state){
                rc = -EINVAL;
                goto error0;
        }
        args = &state->args;
        args->zone = block;
        rc = ioctl(state->sivfs_mod_fd, SIVFS_FREE_BLOCK, args);
        if (rc){
                uout("Here");
                goto error0;
        }

out:

error0:
        return rc;
}

//Must be called before sivfs_alloc and sivfs_mmap on any regions
HEADER_INLINE int sivfs_allocator_add_mmap(void* start, uint64_t tag, size_t size){
        int rc = 0;

        //Initialize allocator state, if necessary
        struct sivfs_allocator_state* state = get_sivfs_allocator_state();
        struct sivfs_allocator_mmap_state* mstate;
        if (!state){
                state =
                        (struct sivfs_allocator_state*) malloc(sizeof(*_sivfs_allocator_state))
                ;
                rc = sivfs_allocator_state_init(state);
                if (rc){
                        free(state);
                        goto error0;
                }
                //Publish the new state
                _sivfs_allocator_state = state;
        }

        //Go through free lists, remove any whose domain overlaps [start,
        //start+size) with tag "tag"
        //
        //We do this so that if you unmap a file, then map a new file on top of
        //the old mmap we won't use the (now invalid) free lists to serve
        //allocations.
        //TODO
        {
        size_t i = 0;
        for(i = 0; i < state->mmap_states.size; i++){
                mstate =
                        (struct sivfs_allocator_mmap_state*) state->mmap_states.values[i]
                ;
                if (sivfs_mmap_state_overlaps(mstate, start, tag, size)){
                        //TODO don't do this - instead require user to manually
                        //close the mmap state along with closing the file, we
                        //can report an error here something like "Allocator
                        //state busy. Prior file was not closed properly?"

                        sivfs_stack_remove(&state->mmap_states, i);
                        sivfs_put_allocator_mmap_state(mstate);
                        /*
                        printf(
                                "Removed existing mmap state @ %p %lu %zd\n",
                                start,
                                tag,
                                size
                        );
                        */
                        i--;
                }
        }
        }

        mstate = sivfs_new_allocator_mmap_state();
        mstate->start = start;
        mstate->tag = tag;
        mstate->size = size;
        rc = sivfs_stack_push(&state->mmap_states, mstate);
        if (rc){
                sivfs_put_allocator_mmap_state(mstate);
                goto error0;
        }

error0:
        return rc;
}

//Gets a reference to the current free list / bin for size "size" in zone
//"zone" with tag "tag". 
//The slot is mutable, i.e. the caller can change the list at that
//slot (necessary if you use the last element in the free list.)
//
//If the returned slot is itself null, it means that the size is too large to
//be handled with free list and "big block" should be used instead.
//
//the size argument refers to the allocating object size, not including
//headers. However, free_list_size includes slab header.
//
//If flags contains SIVFS_ALLOC, if the current free list is NULL we allocate
//one by grabbing a block from the OS. The list is capable of allocating at
//least one element of size "size"
HEADER_INLINE int sivfs_get_free_list_slot(
        size_t* free_list_size,
        struct sivfs_allocator_lh*** slot_out,
        struct sivfs_allocator_state* state,
        struct sivfs_writeset* ws,
        void* zone,
        uint64_t tag,
        size_t size,
        int flags
){
        int rc = 0;

        size_t slab_elt_size;
        struct sivfs_allocator_mmap_state* mmap_state;
        struct sivfs_allocator_lh** slot = NULL;

        if (!size){
                uout("Here");
                rc = -EINVAL;
                goto error0;
        }
        if (size % sizeof(sivfs_word_t)){
                uout("Here");
                rc = -EINVAL;
                goto error0;
        }

        mmap_state = 
                sivfs_get_allocator_mmap_state(state, zone, tag)
        ;
        if (!mmap_state){
                uout("Zone, tag combination did not match any added with add_mmap");
                rc = -EINVAL;
                goto error0;
        }

        slab_elt_size = max2_size_t(
                size + SIVFS_ALLOC_REGION_HEADER_SIZE,
                sizeof(struct sivfs_allocated_region)
        );

        {
        //use bin system and roundup size to the maxsize of the bin.
        size_t binsize;
        struct sivfs_allocator_bin* bin;
        rc = sivfs_allocator_bins_get_bin(
                &binsize,
                &bin,
                &mmap_state->free_lists,
                slab_elt_size
        );
        if (rc){
                uout("Here");
                rc = -EINVAL;
                goto error0;
        }
        if (!bin){
                //No bin, we should do this as a big block
                slot = NULL;
                goto out;
        }

        slot = &bin->free_list;

        //Silly check
        if (slab_elt_size > binsize){
                rc = -EINVAL;
                goto error0;
        }
        //Bump up to bin's maxsize
        slab_elt_size = binsize;
        }

        if (!*slot && (flags & SIVFS_ALLOC)){
                //TODO ask the OS about any global free lists we can claim
        }

        void* block;
        if (!*slot && (flags & SIVFS_ALLOC)){
                size_t block_alloc = SIVFS_BLOCK_HEADER_SIZE + SIVFS_BLOCK_FOOTER_SIZE;

                //blocks must have a nonzero first and last word. Hence the
                //footer around slabs, to protect from coalescing bigblocks
                //into a slab. Slab elements are never coalesced.

                const size_t PAGE_SIZE = 4096;
                /*
                _Static_assert(
                        SIVFS_ALLOCATOR_SLABSIZE >=
                        SIVFS_BLOCK_HEADER_SIZE +
                        SIVFS_BLOCK_FOOTER_SIZE,
                        "Slab size too small"
                );
                _Static_assert(
                        SIVFS_ALLOCATOR_SLABSIZE >=
                        PAGE_SIZE,
                        "Slab size too small"
                );
                */

                size_t slab_elts_size = ROUND_UP(
                        SIVFS_ALLOCATOR_SLABSIZE - block_alloc,
                        slab_elt_size
                );
                size_t n_slab_elts = slab_elts_size / slab_elt_size;

                if (n_slab_elts > SIVFS_MAX_SLAB_ELTS){
                        //Elts per slab exceeds maximum
                        uout("Here");
                        rc = -EINVAL;
                        goto error0;
                }

                block_alloc += slab_elts_size; 

                if (block_alloc & 
                        (SIVFS_ALLOCATION_FLAGS | SIVFS_SLAB_INDEX_MASK)){
                        //Block alloc would be too big - intersects with flags
                        //or slab index
                        uout("Here");
                        rc = -EINVAL;
                        goto error0;
                }

                if (SIVFS_DEBUG_LIB_ALLOC){
                        uout("Allocating block %lu %lu", slab_elt_size, block_alloc);
                }
                void* mmap_start;
                size_t mmap_size;
                size_t block_size;
                rc = sivfs_alloc_block(
                        &mmap_start,
                        &mmap_size,
                        &block,
                        &block_size,
                        zone,
                        block_alloc
                );
                if (rc){
                        goto error1;
                }
                //Some checks
                if (mmap_start != mmap_state->start){
                        uout("Here");
                        rc = -EINVAL;
                        goto error1;
                }
                if (mmap_size != mmap_state->size){
                        uout("Here");
                        rc = -EINVAL;
                        goto error1;
                }

                if (!block_alloc){
                        uout("Here");
                        rc = -EINVAL;
                        goto error1;
                }

                if (block_size < block_alloc){
                        uout("Here");
                        rc = -EINVAL;
                        goto error1;
                }

                //Write the block header and footer
                //The errors below are fatal and should never happen...
                if (rc) goto error1;
                rc = sivfs_write_ws(ws, block, block_size);
                if (rc) goto error1;
                //Footer
                //Write the tag
                rc = sivfs_write_ws(
                        ws,
                        ((char*)block) + block_size - SIVFS_BLOCK_FOOTER_SIZE,
                        tag
                );
                if (rc) goto error1;
                //Then write the size of the block, needed for coalescence
                rc = sivfs_write_ws(
                        ws,
                        ((char*)block) + block_size - SIVFS_BLOCK_FOOTER_SIZE
                                + sizeof(uint64_t),
                        block_size
                );
                if (rc) goto error1;
                //Write the allocated region header
                struct sivfs_allocated_region* free_region =
                (struct sivfs_allocated_region*)(
                        ((char*)block)
                        + SIVFS_BLOCK_HEADER_SIZE
                );
                rc = sivfs_write_ws(
                        ws,
                        &free_region->allocated_size,
                        (block_size
                        - SIVFS_BLOCK_FOOTER_SIZE
                        - SIVFS_BLOCK_HEADER_SIZE) 
                        | SIVFS_FREED_MAGIC | SIVFS_LIST_HEAD_FLAG
                );
                if (rc) goto error1;
                rc = sivfs_write_wsp(ws, &free_region->lh.next, &free_region->lh);
                if (rc) goto error1;
                rc = sivfs_write_wsp(ws, &free_region->lh.prev, &free_region->lh);
                if (rc) goto error1;

                //Set our free list for this bin
                rc = sivfs_set_free_list(slot, &free_region->lh);
                if (rc) goto error1;
        }

out:
        *slot_out = slot;
        *free_list_size = slab_elt_size;
        return rc;

error1:
        //assert(0);

error0:
        return rc;
}

//The actual allocation routine
//zone need only be a pointer to anywhere in the mmap that the allocation
//should occur from. tag is used for tagging allocations within the file,
//mostly useful for recovery schemes that involve walking the heap (the "tag"
//can be used as a kind of type system)
HEADER_INLINE void* sivfs_malloc_ws(
        void* zone,
        uint64_t tag,
        size_t size,
        struct sivfs_writeset* ws
){
        void* toRet = NULL;
        struct sivfs_allocator_state* state = get_sivfs_allocator_state();
        if (!state){
                fprintf(stderr, "ERROR: Malloc called before allocator_add_mmap\n");
                assert(0);
                exit(1);
        }

        size_t free_list_size;
        struct sivfs_allocator_lh** free_list;
        struct sivfs_allocated_region* allocate_within = NULL;
        int rc = 0;

        //TODO I'm being too careful here, really we can support non-8-byte
        //aligned writes, and hence allocations, with some minor flexibility in
        //the log (a stream operator that says that the following writes are
        //single-byte-writes?) and everything works out fine. Still, for
        //simplicity, just stick to 8-byte-aligned things for now.

        /*
        if (size % sizeof(sivfs_word_t)){
                fprintf(stderr, "ERROR: Malloc should have 8-byte aligned size, for now.\n");
                assert(0);
                dout("Here");
                goto error0;
        }
        */
        size = ROUND_UP(size, sizeof(sivfs_word_t));

        //Will lookup a free list in the freelists of state, otherwise grab a
        //new one with OS help. If size is too large, we'll fail here and see
        //below.
        rc = sivfs_get_free_list_slot(
                &free_list_size,
                &free_list,
                state,
                ws,
                zone,
                tag,
                size,
                SIVFS_ALLOC
        );
        if (rc){
                uout("Here");
                goto error0;
        }

        if (!free_list){
                //size is too big to use free lists, allocate as a big block.
                void* mmap_start;
                size_t mmap_size;
                void* block;
                size_t actual_block_size;
                size_t block_size = 
                max2_size_t(
                        size 
                        + SIVFS_BLOCK_HEADER_SIZE 
                        + SIVFS_BLOCK_FOOTER_SIZE,
                        sizeof(struct sivfs_allocated_region)
                        + SIVFS_BLOCK_FOOTER_SIZE
                );

                if (block_size & SIVFS_ALLOCATION_FLAGS){
                        uout("Here");
                        rc = -EINVAL;
                        goto error0;
                }

                rc = sivfs_alloc_block(
                        &mmap_start,
                        &mmap_size,
                        &block,
                        &actual_block_size,
                        zone,
                        block_size
                );
                if (rc){
                        goto error0;
                }
                if (block_size > actual_block_size){
                        uout("Here %lu %lu", block_size, actual_block_size);
                        rc = -EINVAL;
                        goto error0;
                }

                //For whatever reason, the kernel gave us a slightly larger
                //block. 
                block_size = actual_block_size;

                struct sivfs_allocated_region* free_region;
                free_region = (struct sivfs_allocated_region*)block;
                
                rc = sivfs_write_ws(
                        ws,
                        &free_region->allocated_size,
                        block_size | SIVFS_BIG_ALLOCATION_FLAG
                );
                if (rc) goto error0;
                //write the tag into the footer
                rc = sivfs_write_ws(
                        ws,
                        ((char*)block) + block_size - SIVFS_BLOCK_FOOTER_SIZE,
                        tag
                );
                if (rc) goto error0;
                //write out the size in the footer as well for coalescence
                //support
                rc = sivfs_write_ws(
                        ws,
                        ((char*)block) + block_size - SIVFS_BLOCK_FOOTER_SIZE
                                + sizeof(uint64_t),
                        block_size
                );
                if (rc) goto error0;

                rc = sivfs_write_wsp(ws, &free_region->lh.next, &free_region->lh);
                if (rc) goto error0;
                rc = sivfs_write_wsp(ws, &free_region->lh.prev, &free_region->lh);
                if (rc) goto error0;

                allocate_within = free_region;
                goto out;
        }

        if (!*free_list){
                //Assertion error
                uout("Here");
                rc = -ENOMEM;
                goto error0;
        }

        //Make a tentative change to the free list, if possible.

        //Iterate over free list and allocate memory
        {
        struct sivfs_allocator_lh* iter;
        struct sivfs_allocator_lh* head = *free_list;

        //Sanity check
        {
                struct sivfs_allocated_region* region = 
                        sivfs_allocator_lh_to_allocated_region(head)
                ;
                if ((region->allocated_size & SIVFS_FREED_MAGIC_MASK) != SIVFS_FREED_MAGIC){
                        fprintf(stderr, "Heap corruption: "
                        "Free list head was not free at %p\n"
                        , region);
                        assert(0);
                        exit(1);
                }
        }


        //Inclusive iteration, goes over the free list returning the head
        //element last.
        split_head:
        sivfs_alloc_lh_for_each_incl(iter, head){
                struct sivfs_allocated_region* region =
                        sivfs_allocator_lh_to_allocated_region(iter)
                ;

                if ((region->allocated_size & SIVFS_FREED_MAGIC_MASK) != SIVFS_FREED_MAGIC){
                        fprintf(stderr, "Heap corruption: "
                        "Trying to allocate from region without "
                        "magic free word at %p\n", region);
                        assert(0);
                        exit(1);
                }

                //TODO check if region has enough room
                allocate_within = region;

                //The item may be larger than the free_list_size, split it
                //appropriately into chunks of free_list_size.
                bool split = false;

                size_t region_size = sivfs_allocated_region_size(region);

                if (region_size >= free_list_size * 2){
                        //Split
                        struct sivfs_allocated_region* split_next;
                        split_next = (struct sivfs_allocated_region*)(
                                ((char*)region) + free_list_size
                        );
                        size_t split_next_size = 
                                (region_size - free_list_size)
                        ;
                        region_size = free_list_size;

                        //slab indices consistent because the region being
                        //split is always the one with maximum slab index
                        uint64_t split_slab_index =
                                sivfs_allocated_region_slab_index(region)
                                + 1
                        ;
                        if (split_slab_index > SIVFS_MAX_SLAB_ELTS){
                                dout("Here");
                                rc = -EINVAL;
                                goto error0;
                        }

                        //update region's size
                        // - maintain flags
                        // - maintain slab index
                        rc = sivfs_write_ws(
                                ws,
                                &region->allocated_size,
                                region_size
                                | sivfs_allocated_region_flags(region)
                                | (region->allocated_size & SIVFS_SLAB_INDEX_MASK)
                        );
                        if (rc) goto error0;

                        //Add the split element to the free list
                        rc = sivfs_write_ws(
                                ws,
                                &split_next->allocated_size,
                                split_next_size
                                | SIVFS_FREED_MAGIC
                                | (split_slab_index << SIVFS_SLAB_INDEX_SHIFT)
                        );
                        if (rc) goto error0;

                        //Add the split element to the free list
                        rc = sivfs_alloc_lh_add_ws(
                                ws,
                                head,
                                &split_next->lh
                        );
                        if (rc) goto error0;

                        if (iter == head){
                                //Restart iteration since we split head
                                goto split_head;
                        }
                }

                if (region_size < free_list_size){
                        //Assertion error
                        rc = -EINVAL;
                        goto error0;
                }

                if (region_size > free_list_size){
                        //Ok, here we need to set region_size to be exactly
                        //free_list_size so that when it is freed it goes back
                        //here. This means we permanently waste a bit of space
                        //here, up to one allocation per slab.
                        region_size = free_list_size;
                }
                //Set region's allocated size and clear flags
                //we make sure to maintain slab index of region
                rc = sivfs_write_ws(
                        ws,
                        &region->allocated_size,
                        region_size
                        | (region->allocated_size & SIVFS_SLAB_INDEX_MASK)
                );
                if (rc) goto error0;

                if (iter == head) {
                        //Then allocate the head element. This must be the last
                        //element we allocate.
                        if (!sivfs_alloc_list_empty(head)){
                                //Assertion error
                                uout("Here");
                                goto error0;
                        }
                        
                        //Free the reference to the list.
                        //Tif we end up aborting, we should put
                        //back the free list. 
                        rc = sivfs_set_free_list(free_list, NULL);
                        if (rc) goto error0;
                } else {
                        //Then remove the region from the free list
                        rc = sivfs_alloc_lh_remove_ws(ws, &region->lh);
                        if (rc) goto error0;
                }

                break;
        }
        }

        if (!allocate_within){
                //Couldn't satisfy request. Actually, we shouldn't get here
                //because get_free_list should return a free list that has
                //enough capacity to satisfy the allocation. But it's a double
                //check.
                uout("Here %lu %p", size, free_list);
                goto error0;
        }

out:
        //Return the allocation
        toRet = sivfs_allocated_region_to_alloc_ret(allocate_within);
        if (SIVFS_DEBUG_LIB_ALLOC){
                uout("Allocated %p", toRet);
        }

        state->stats.nAllocs++;

error0:
        return toRet;
}

HEADER_INLINE int sivfs_free_ws(void* _ptr, struct sivfs_writeset* ws){
        int rc = 0;

        //Move ptr backwards to account for header word
        struct sivfs_allocated_region* ptr = sivfs_alloc_ret_to_region(_ptr);

        struct sivfs_allocator_state* state;

        state = get_sivfs_allocator_state();
        if (!state){
                //error, since caller should call add_mmap at least once first.
                assert(0);
                exit(1);
        }

        //Check double free
        if ((ptr->allocated_size & SIVFS_FREED_MAGIC_MASK) == 0){
                //OK!
        } else if (
                (ptr->allocated_size & SIVFS_FREED_MAGIC_MASK) 
                == SIVFS_FREED_MAGIC
        ){
                uout("Double free!");
                assert(0);
                exit(1);
        } else {
                uout("Attempt to free an invalid object (or heap corruption)");
                assert(0);
                exit(1);
        }

        size_t allocated_size = 
                sivfs_allocated_region_size(ptr)
        ;

        size_t object_size, flags;
        if (ptr->allocated_size & SIVFS_BIG_ALLOCATION_FLAG){
                //Just pass it to sivfs_free_block
                rc = sivfs_free_block(ptr);
                if (rc){
                        uout("Here");
                        goto error0;
                }

                //TODO zero out the whole range too (only needed for recovery)
                size_t* footer_ptr = (size_t*)(
                        ((char*)ptr) 
                        + allocated_size
                        - SIVFS_BLOCK_FOOTER_SIZE
                );
                rc = sivfs_write_ws(ws, &ptr->allocated_size, 0);
                if (rc) goto error0;
                rc = sivfs_write_ws(ws, footer_ptr, 0);
                if (rc) goto error0;
                rc = sivfs_write_ws(ws, footer_ptr + sizeof(uint64_t), 0);
                if (rc) goto error0;

                //Done.
                goto out;
        }

        if (allocated_size == 0){
                //Not a valid allocation of any kind
                uout("ERROR: Possibly heap corruption." 
                "Attempt to free invalid chunk.");
                rc = -EINVAL;
                goto error0;

        }

        //Otherwise, it's a slab object

        //Derive object size from allocated size, since object_size is the
        //interface to get_free_list
        object_size =
                allocated_size - SIVFS_ALLOC_REGION_HEADER_SIZE
        ;

        if (SIVFS_DEBUG_LIB_ALLOC){
                uout("Freeing %p", _ptr);
        }
        if ((allocated_size & SIVFS_FREED_MAGIC_MASK) == SIVFS_FREED_MAGIC){
                fprintf(stderr, "Double free detected at %p", _ptr);
                assert(0);
                exit(1);
        }
        if (allocated_size & SIVFS_FREED_MAGIC_MASK){
                fprintf(stderr, "Corrupt allocated element detected at %p", _ptr);
                assert(0);
                exit(1);
        }

        //Slab head lookup
        void* slab_head;
        uint64_t tag;
        slab_head = sivfs_alloc_region_to_block_head(ptr);
        if (!slab_head){
                fprintf(stderr, "Couldn't look up slab head for slab elt %p", _ptr);
                assert(0);
                exit(1);
        }
        tag = sivfs_block_tag(slab_head); 

        //Will lookup a free list in the freelists of state. If none found, we
        //query the OS for the [start, size) of the mmap containing ptr to
        //construct free_list but we don't fill in free_list->free_list.
        //will start our own @ ptr.

        size_t free_list_size;
        struct sivfs_allocator_lh** free_list;
        rc = sivfs_get_free_list_slot(
                &free_list_size,
                &free_list,
                state,
                ws,
                ptr,
                tag,
                object_size,
                0
        );
        if (rc){
                goto error0;
        }
        if (!free_list){
                //Assertion error!
                rc = -EINVAL;
                goto error0;
        }

        flags = SIVFS_FREED_MAGIC;
        if (!*free_list){
                //Starting a new free list.
                rc = sivfs_write_wsp(ws, &ptr->lh.prev, &ptr->lh);
                if (rc) goto error0;
                rc = sivfs_write_wsp(ws, &ptr->lh.next, &ptr->lh);
                if (rc) goto error0;
                flags |= SIVFS_LIST_HEAD_FLAG;
                
                rc = sivfs_set_free_list(free_list, &ptr->lh);
                if (rc) goto error0;
        } else {
                //append onto existing free list
                rc = sivfs_alloc_lh_add_ws(ws, *free_list, &ptr->lh);
                if (rc) goto error0;
        }

        rc = sivfs_allocated_region_set_flags(
                ws,
                ptr,
                flags
        );
        if (rc){
                uout("Here");
                goto error0;
        }

        //TODO Must revert changes to bin free_list pointers if we abort.

out:
        state->stats.nFrees++;

error0:
        return rc;
}

#endif
