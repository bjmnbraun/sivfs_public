#ifndef SIVFS_WRITESET_H
#define SIVFS_WRITESET_H

#include "sivfs_common.h"
#include "sivfs_types.h"
#include "sivfs_config.h"

#ifndef __KERNEL__
#include <algorithm>
#include <errno.h>
#include <assert.h>
#include <iostream>
//##__VA_ARGS__ needed to avoid failure when no args
#define dout(format, ...) printf(format "\n", ##__VA_ARGS__)
#else
#include <linux/slab.h>
#include <linux/highmem.h>
#endif

#define DEFINE_WRITESET_ENTRY(_name, _address,_value) \
        struct sivfs_writeset_entry _name \
        { \
                .ino=0, \
                .file_offset=0, \
                .address=(unsigned long)_address, \
                .stream_control=0, \
                .value=_value \
        }

//Stream extension
#define SIVFS_ENTRY_IS_STREAM (1UL << 63)
//A special kind of stream where value is duplicated for the length of the
//stream (almost always used to zero out large portions of files)
#define SIVFS_STREAM_CONSTANT (1UL << 62)

#define SIVFS_STREAM_FLAGS (SIVFS_ENTRY_IS_STREAM | SIVFS_STREAM_CONSTANT)

struct sivfs_writeset_entry {
        //Used only by kernel
        unsigned long ino;
        size_t file_offset;

        //Address. Kept in log just for debugging purposes.
        unsigned long address;

        //Stream control - 64 bits see flags above
        unsigned long stream_control;

        //New value. Must be last field of struct.
        sivfs_word_t value;
};

#define SIVFS_WRITESET_ENTRY_MIN_SIZE sizeof(struct sivfs_writeset_entry)

//Unpacked form of stream_control, above
struct sivfs_wse_stream_info {
        unsigned long value_words;
        unsigned long dest_words;
        bool is_stream;
        bool is_const_stream;
};

HEADER_INLINE size_t sivfs_wse_size(struct sivfs_wse_stream_info* info){
        return sizeof(struct sivfs_writeset_entry) +
                (info->value_words - 1) * sizeof(sivfs_word_t)
        ;
}

//Also does a size check, the entry's value field should not extend past
//size bytes from entry
HEADER_INLINE int sivfs_wse_to_stream_info(
        struct sivfs_wse_stream_info * info,
        struct sivfs_writeset_entry* entry,
        size_t size
) {
        unsigned long sc = entry->stream_control;
        //Common case, likely.
        if (sc == 0){
                *info = (struct sivfs_wse_stream_info) {1,1,0,0};
                return 0;
        }

        int rc = 0;
        bool is_stream = sc & SIVFS_ENTRY_IS_STREAM;
        bool is_const_stream = sc & SIVFS_STREAM_CONSTANT;
        size_t dest_words = sc & ~SIVFS_STREAM_FLAGS;
        if (!is_stream){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }
        if (dest_words == 0){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }
        if (is_const_stream){
                info->value_words = 1;
        } else {
                info->value_words = dest_words;
        }
        info->dest_words = dest_words;
        info->is_stream = is_stream;
        info->is_const_stream = is_const_stream;

        //Size check
        {
                size_t actual_size = sivfs_wse_size(info);
                if (actual_size > size){
                        dout("Size error");
                        rc = -EINVAL;
                        goto error0;
                }
        }

error0:
        return rc;
}

//Zero initialization currently OK
struct sivfs_writeset {
        //TODO rewrite the code with __size_bytes and then rename to just size
        //later
        //Size, in bytes, of entries allocated
        size_t __capacity_bytes;
        //Number of bytes of entries actually used
        size_t __size_bytes;
        struct sivfs_writeset_entry* entries;
};

HEADER_INLINE bool sivfs_writeset_empty(struct sivfs_writeset* ws){
        return ws->__size_bytes == 0;
}

//Tries to reserve enough space for new_capacity. If no memory,
//ws is unmodified and an error is returned.
HEADER_INLINE int sivfs_writeset_reserve(
        struct sivfs_writeset* ws,
        size_t new_capacity
){
        int rc = 0;
        if (new_capacity >= ws->__capacity_bytes){
                //reallocate
                new_capacity = max3_size_t(
                        ws->__capacity_bytes * 2,
                        4 * SIVFS_WRITESET_ENTRY_MIN_SIZE,
                        new_capacity
                );
                struct sivfs_writeset_entry* newalloc;
#ifdef __KERNEL__
                newalloc = kzalloc(
                        new_capacity,
                        GFP_KERNEL
                );

                if (!newalloc){
                        rc = -ENOMEM;
                        goto error0;
                }
                struct sivfs_writeset_entry* old_entries = ws->entries;
                memcpy(
                        newalloc,
                        ws->entries,
                        ws->__size_bytes
                );

                //Hmm. Use object cache here?
                kfree(old_entries);
#else //ifdef __KERNEL__
                newalloc = reinterpret_cast<decltype(newalloc)>( realloc(
                        ws->entries,
                        new_capacity
                ) );
                if (!newalloc){
                        rc = -ENOMEM;
                        goto error0;
                }
#endif //ifndef __KERNEL__
                ws->__capacity_bytes = new_capacity;
                ws->entries = newalloc;
        }

error0:
        return rc;
}

#define SIVFS_WRITESET_DISABLED_FLAG (1ULL<<63)
HEADER_INLINE int sivfs_disable_ws(struct sivfs_writeset* ws, bool disable){
        int rc = 0;
        if (disable){
                if(ws->__size_bytes != 0){
                        rc = -EINVAL;
                        goto error0;
                }
                ws->__size_bytes |= SIVFS_WRITESET_DISABLED_FLAG; 
        } else {
                ws->__size_bytes &= ~SIVFS_WRITESET_DISABLED_FLAG; 
                if(ws->__size_bytes != 0){
                        rc = -EINVAL;
                        goto error0;
                }
        }

error0:
        return rc;
}
HEADER_INLINE bool sivfs_ws_is_disabled(struct sivfs_writeset* ws){
        return 0 != (ws->__size_bytes & SIVFS_WRITESET_DISABLED_FLAG);
}

//size is the size in bytes of toAdd
HEADER_INLINE int sivfs_add_to_writeset(
        struct sivfs_writeset* ws,
        struct sivfs_writeset_entry* toAdd,
        size_t size
){
        int rc = 0;

        if (sivfs_ws_is_disabled(ws)){
                return 0;
        }

        struct sivfs_wse_stream_info info;
        rc = sivfs_wse_to_stream_info(
                &info,
                toAdd,
                size
        );
        if (rc){
                dout("Here");
                goto error0;
        }

        {
                size_t wse_size = sivfs_wse_size(&info);
                size_t new_size = ws->__size_bytes + wse_size;

                //Note that this check is also done in the kernel module
                if (new_size > MAX_COMMIT_SIZE / 2 && new_size <
                MAX_COMMIT_SIZE / 2 + 16){
                        dout(
                                "WARN: Large commit(%zd)! Increase MAX_COMMIT_SIZE",
                                new_size
                        );
                }

                rc = sivfs_writeset_reserve(ws, new_size);
                if (rc) {
                        goto error0;
                }
                //Copy over entry
                memcpy((char*)ws->entries + ws->__size_bytes,
                        toAdd,
                        wse_size
                );
                ws->__size_bytes = new_size;
        }

error0:
        return rc;
}

#ifdef __KERNEL__

//Assumes dest->capacity >= src->size,
//then copies src into dest and sets dest's size.
HEADER_INLINE void sivfs_writeset_copy(
        struct sivfs_writeset* dest,
        struct sivfs_writeset* src
){
        if (dest->__capacity_bytes < src->__size_bytes){
                dout("Assertion error");
                //No good recovery here, this shouldn't happen
                dest->__size_bytes = 0;
                return;
        }
        memcpy(dest->entries, src->entries, src->__size_bytes);
        dest->__size_bytes = src->__size_bytes;
}

HEADER_INLINE void sivfs_apply_ws_ent_pg(
        void* mem,
        struct sivfs_writeset_entry* ent
){
        sivfs_word_t* word =
        (void*)(
                (char*)mem +
                (ent->file_offset % PAGE_SIZE)
        );
        *word = ent->value;
}

#endif //ifdef __KERNEL__

//Don't release dout
#ifndef __KERNEL__
#undef dout
#endif

#endif
