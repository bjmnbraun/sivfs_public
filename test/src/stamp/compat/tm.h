#pragma once

#include "txsys.h"

#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>

///Compatibility functions for STAMP
#define TM_SAFE
#define TM_PURE

//TODO the next few methods need to be implemented.

void* const stamp_shared_mmap_addr = (void*)(0xE00ULL<<32ULL);
void* const stamp_allocation_zone = stamp_shared_mmap_addr;
const size_t stamp_fsize = 0x10000000000;

HEADER_INLINE int stamp_open_shared_file(
        const char* _filename,
        bool only_create
){
        std::string filename =
                std::string("/mnt/sivfs") + std::string("/") + std::string(_filename)
        ;
        void* address = stamp_shared_mmap_addr;

        int fhandle = open(
                filename.c_str(),
                O_RDWR | (only_create? (O_TRUNC | O_CREAT) : 0),
                0600
        );
        assert(fhandle >= 0);
        int mapflags = MAP_SHARED | MAP_FIXED;
        //TODO right now we create very large files, but really this makes
        //debugging harder because fixed-size files should segfault if we
        //address past them.
        //
        //2^40 = (12+12+12+9)*2
        int rc = 0;
        if (only_create){
                rc = ftruncate(fhandle, stamp_fsize);
                assert(rc == 0);
                close(fhandle);
                return rc;
        }
        //Munmap any existing map there.
        rc = munmap(address, stamp_fsize);
        assert(rc == 0);
        void* ptr = (char*)mmap(
                address,
                stamp_fsize,
                PROT_READ | PROT_WRITE,
                mapflags,
                fhandle,
                0
        );
        assert(ptr != MAP_FAILED);

        //Set up allocator. Ideally, we don't move around an mmap after calling
        //this, but if we must we can by calling add_mmap again (which will
        //invalidate some cache entries in the allocator.)
        //
        //We also tag all allocations at 0
        rc = sivfs_allocator_add_mmap(address, 0, stamp_fsize);
        assert(rc == 0);

        close(fhandle);

        return rc;
}

HEADER_INLINE int TM_STARTUP() {
        int rc = 0;

        sivfs_init();

        rc = stamp_open_shared_file("stamp", true);
        assert(rc == 0);
        return rc;
}

HEADER_INLINE int TM_THREAD_ENTER() {
        int rc = 0;
        rc = stamp_open_shared_file("stamp", false);
        assert(rc == 0);

        return rc;
}

//Just syntax sugar
#define TM_BEGIN_INITIALIZATION TM_BEGIN_DIRECT_ACCESS
#define TM_END_INITIALIZATION TM_END_DIRECT_ACCESS

HEADER_INLINE int TM_TXN_PAUSE() {
        //TODO
        return 0;
}

//Called only when retries >= 1
HEADER_INLINE bool stamp_txn_atomic_loop(size_t retries){
        if (retries > 100){
                fprintf(stderr, "Transaction retried >= 100 times!");
                assert(0);
        }
        bool aborted;
        TM_COMMIT_UPDATE(aborted);
        //If we aborted, we want to repeat the loop.
        return aborted;
}

#define __transaction_atomic {bool __aborted; TM_COMMIT_UPDATE(__aborted);} \
        for(size_t __retries = 0; \
        __retries == 0 || stamp_txn_atomic_loop(__retries); \
        __retries++)

//transaction_cancel is a non-control-flow-changing abort. Another way to
//implement it is to set a flag so that the __transaction_atomic aborts instead
//but that adds a conditional to each __transaction_atomic in the usual case.
#define __transaction_cancel TM_ABORT()

//Should close the currently open mmap. Next commit will re-open it.
//We use this when a thread is going to sleep for a long time, and when the
//driver thread is done with initialization of the file.
#define TM_SHUTDOWN() {}

//Do a rename trick here to change the semantics of TM_READ to operate on
//lvalue references (what stamp expects) instead of pointers-to-data (what
//the current tx_sys code exposes)
template <class T>
HEADER_INLINE T _TM_READ(const T& X){
        return TM_READ(&X);
}

//TODO this only works for sitevm backend, and possibly works for genome in all
//cases just because when this is used in genome, there are no concurrent
//writers to the sequence. Either way, best to avoid using TM_STRNCMP in new
//applications.
#define TM_STRNCMP strncmp

template <class T>
HEADER_INLINE void _TM_WRITE(T& X, T Y){
        return TM_WRITE(&X, (uint64_t) Y);
}

#undef TM_READ
#undef TM_WRITE

#define TM_SHARED_READ(var) _TM_READ(var)
#define TM_SHARED_READ_P(ptr) _TM_READ(ptr)
#define TM_SHARED_READ_F(var) _TM_READ(var)

#define TM_SHARED_WRITE(var, value) _TM_WRITE(var, value)
#define TM_SHARED_WRITE_P(ptr, value) _TM_WRITE(ptr, value)
#define TM_SHARED_WRITE_F(var, value) _TM_WRITE(var, value)

#define TM_LOCAL_WRITE(var, value) _TM_WRITE(var, value)
#define TM_LOCAL_WRITE_P(ptr, value) _TM_WRITE(ptr, value)
#define TM_LOCAL_WRITE_F(var, value) _TM_WRITE(var, value)

//Supply the one and only zone, since stamp apps use a single file
//also use 0 tag for all allocations
//Override existing TM_MALLOC macro

HEADER_INLINE void* _TM_MALLOC(size_t size){
        return TM_MALLOC(stamp_allocation_zone, 0, size);
}

#undef TM_MALLOC
#define TM_MALLOC(size) _TM_MALLOC(size)

HEADER_INLINE void TM_MEMSET_ZERO(void* _ptr, size_t _n){
        //memset(toRet, 0, size*n);

        assert(_n % sizeof(sivfs_word_t) == 0);
        size_t n = _n / sizeof(sivfs_word_t);

        size_t i;
        sivfs_word_t* ptr = (sivfs_word_t*)_ptr;
        for(i = 0; i < n; i++){
                TM_SHARED_WRITE(ptr[i], (sivfs_word_t)0);
        }
}

HEADER_INLINE void* TM_CALLOC(size_t n, size_t size){
        void* toRet = TM_MALLOC(size*n);
        if (toRet){
                TM_MEMSET_ZERO(toRet, size*n);
        }
        return toRet;
}

//Just do a null transaction to achieve a "sync"
#define TM_SYNC() __transaction_atomic{}

#define SEQ_MALLOC(size) TM_MALLOC(size)
#define SEQ_FREE(ptr) TM_FREE(ptr)

//Some stamp benchmarks store function pointers in transactional space.
//In general, this is a bad plan because recompiling invalidates the
//transactional space (because the vtable pointers might be wrong.)
//
//It also makes it harder to share such objects with other languages that might
//not understand vtables.
//
//Regardless, it's safe for us to do this for the stamp benchmarks because
//there we have a driver process that forks workers:
#define TM_IFUNC_CALL1(retVal, funcPtr, arg) retVal = funcPtr(arg);
#define TM_IFUNC_CALL2(retVal, funcPtr, arg1, arg2) retVal = funcPtr((arg1), (arg2));

