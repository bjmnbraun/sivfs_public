#ifndef SIVFS_LOG_H
#define SIVFS_LOG_H

#include "sivfs_writeset.h"

//Represents an entry in the sivfs_log
struct sivfs_log_entry {
        //Not strictly necessary.
        sivfs_ts ts;

        struct sivfs_writeset ws;

        //Commit status
        bool aborted;
        //Whether the aborted field is filled in or not
        bool commit_status_filled_in;

        //Whether or not ws has its metadata tagged
        bool ws_has_metadata;

        //Sequential allocation optimization relevant...
        //Whether the log entry is the first in an allocated chcunk of log
        bool first_in_chunk;

        //Doubly linked list
        struct sivfs_log_entry* prev;
        struct sivfs_log_entry* next;
};

struct sivfs_log {
        //The latest entry in the log
        struct sivfs_log_entry* latest;

        //Sequential allocation optimization relevant...
        //How many bytes left in the last allocated chunk
        size_t bytes_remaining_chunk;
        //Pointer to the next free byte in the last allocated chunk (or null)
        void* chunk_ptr;
        //Pointer to the first byte in the last allocated chunk chunk (or null)
        void* chunk_start_ptr;
};

HEADER_INLINE int sivfs_init_log(struct sivfs_log* log){
        //Init should begin with a zeroing for safety
        memset(log, 0, sizeof(*log));
        return 0;
}
HEADER_INLINE void sivfs_destroy_log(
        struct sivfs_log* log
){
        //We currently erase log entries when we clean up the log_cache in
        //sivfs_shared_state.
}

HEADER_INLINE int sivfs_init_log_entry(struct sivfs_log_entry* log_entry){
        int rc = 0;

        memset(log_entry, 0, sizeof(*log_entry));
        log_entry->ts = SIVFS_INVALID_TS;

        return rc;
}

HEADER_INLINE void sivfs_destroy_log_entry(struct sivfs_log_entry* log_entry){
        kfree(log_entry->ws.entries);
}

HEADER_INLINE int sivfs_new_log_entry(struct sivfs_log_entry** new_log_entry){
        int rc = 0;

        //TODO switch to an object pool. This will come with the revamped log
        //structuring.
        struct sivfs_log_entry* log_entry;
        log_entry = kzalloc(sizeof(*log_entry), GFP_KERNEL);

        if (!log_entry){
                rc = -ENOMEM;
                goto error0;
        }

        rc = sivfs_init_log_entry(log_entry);
        if (rc){
                goto error0;
        }

out:
        *new_log_entry = log_entry;
        return rc;

error0:
        return rc;
}

//Used for freeing the result of sivfs_new_log
//Handles null inputs just fine (they are ignored).
HEADER_INLINE void sivfs_put_log_entry(struct sivfs_log_entry* log_entry){
        if (log_entry){
                sivfs_destroy_log_entry(log_entry);
        }
        kfree(log_entry);
}


#endif
