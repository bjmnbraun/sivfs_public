#pragma once

#include <fcntl.h>
#include <sys/ioctl.h>
#include <cassert>
#include <cstdint>
#include <unistd.h>

#include "sivfs.h"
#include "sivfs_allocator.h"
#include "common.h"

extern int sivfs_mod_fd;
extern struct sivfs_args_t sivfs_args;

HEADER_INLINE void sivfs_init(){
        sivfs_mod_fd = open("/dev/sivfs", O_RDWR);
        assert(sivfs_mod_fd != -1);

        //Prepare args. Fork will copy a properly initialized version
        sivfs_init_args(&sivfs_args);
}

HEADER_INLINE void sivfs_get_stats(struct sivfs_stats* stats){
        sivfs_args.stats = stats;
        int rc = ioctl(sivfs_mod_fd, SIVFS_GET_STATS, &sivfs_args);
        assert(rc == 0);
}

HEADER_INLINE void sivfs_commit_update(/*out*/ bool& aborted){
        int rc = ioctl(sivfs_mod_fd, SIVFS_TXN_COMMIT_UPDATE, &sivfs_args);
        assert(rc == 0);
        aborted = sivfs_args.aborted;

        rc = sivfs_lib_allocation_on_commit(aborted);
        assert(rc == 0);
}

HEADER_INLINE void sivfs_abort_update(){
        sivfs_args.abort = true;
        int rc = ioctl(sivfs_mod_fd, SIVFS_TXN_COMMIT_UPDATE, &sivfs_args);
        assert(rc == 0);
        sivfs_args.abort = false;

        rc = sivfs_lib_allocation_on_commit(true);
        assert(rc == 0);
}

HEADER_INLINE void sivfs_begin_direct_access(){
        sivfs_args.wants_direct_access = true;

        //XXX this retry loop needs to happen in the OS as otherwise the 
        //interface semantics are weird, see below for confusion on whether
        //committing allocation is safe
        int retries = 0;
retry:
        int rc = ioctl(sivfs_mod_fd, SIVFS_TXN_COMMIT_UPDATE, &sivfs_args);
        if (rc){
                if (retries > 0){
                        printf("Waiting to start direct access...\n");
                }
                sleep(1);
                retries++;
                goto retry;
        }
        assert(rc == 0);

        bool aborted = sivfs_args.aborted;
        assert(!aborted);

        //Note that we are unsure here whether the allocations were aborted
        //due to the retry loop! That retry loop needs to happen in the OS...
        rc = sivfs_lib_allocation_on_commit(aborted);
        assert(rc == 0);

        //Need to prevent sivfs_write from modifying writeset unnecessarily
        rc = sivfs_disable_ws(&sivfs_args.ws, true);
        assert(rc == 0);
}

HEADER_INLINE void sivfs_end_direct_access(){
        //Re-enable writeset
        int rc = sivfs_disable_ws(&sivfs_args.ws, false);
        assert(rc == 0);
        sivfs_args.wants_direct_access = false;

        bool aborted;
        sivfs_commit_update(aborted);
        assert(!aborted);
}

HEADER_INLINE void sivfs_write(volatile void* address, uint64_t value){
        int rc = sivfs_write_ws(&sivfs_args.ws, address, value);
        assert(rc == 0);
}

HEADER_INLINE void sivfs_write(volatile void* address, long int value){
        int rc = sivfs_write_ws(&sivfs_args.ws, address, value);
        assert(rc == 0);
}

HEADER_INLINE void sivfs_write(volatile void* address, void* value){
        int rc = sivfs_write_wsp(&sivfs_args.ws, address, value);
        assert(rc == 0);
}

//Note - must call sivfs_allocator_add_mmap before malloc / freeing from any
//sivfs mmap
HEADER_INLINE void* sivfs_malloc(void* zone, uint64_t tag, size_t size){
        void* toRet = sivfs_malloc_ws(zone, tag, size, &sivfs_args.ws);
        assert(toRet);
        return toRet;
}

HEADER_INLINE void sivfs_free(void* ptr){
        int rc = sivfs_free_ws(ptr, &sivfs_args.ws);
        assert(rc == 0);
}

