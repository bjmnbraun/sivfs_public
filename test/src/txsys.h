#pragma once

#include <stdint.h>
#include <stdexcept>

#include "common.h"
#include "txsys_config.h"

//exception thrown when a write or read operation causes abort and the
//transaction must not be continued
//
//Not all tx_sys throw this

//////
#if TX_SYS == TX_SYS_SIVFS

#include "sivfs.h"

#include "tx_sys/tx_sys_sivfs.h"

#define TX_SYS_STATS SIVFS_STATS
typedef struct sivfs_stats tx_sys_stats_t;

#define tx_sys_get_stats sivfs_get_stats

#define TX_SYS_ALLOCATOR_STATS SIVFS_ALLOCATOR_STATS
typedef struct sivfs_allocator_stats tx_sys_allocator_stats_t;

#define tx_sys_get_allocator_stats sivfs_get_allocator_stats

#define TM_WRITE sivfs_write
#define TM_READ(X) *(X)
#define TM_COMMIT_UPDATE sivfs_commit_update
#define TM_ABORT sivfs_abort_update
#define TM_BATCH_END sivfs_abort_update

#define TM_MALLOC(zone, tag, size) sivfs_malloc(zone, tag, size)
#define TM_FREE(ptr) sivfs_free(ptr)

#define TM_BEGIN_DIRECT_ACCESS() sivfs_begin_direct_access()
#define TM_END_DIRECT_ACCESS() sivfs_end_direct_access()

typedef int TM_ABORTED_EXCEPTION;

#define TX_SYS_INCONSISTENT_READS false

//////
#elif TX_SYS == TX_SYS_GL

#include "tx_sys/gl.h"

#define TX_SYS_STATS GL_STATS
typedef struct gl_stats tx_sys_stats_t;

#define TM_COMMIT_UPDATE gl_commit_update

#define tx_sys_get_stats gl_get_stats

//Just do the write (and pray that we are actually in a txn!)
//Note that one advantage of our virtual memory based STM is that writes outside of transactions
//are OK and just impact the local working set and will be committed safely (or with a warning)

HEADER_INLINE void TM_WRITE(volatile void* address, uint64_t value){
        ((uint64_t*)address)[0] = value;
}

HEADER_INLINE void TM_WRITE(volatile void* address, void* value){
        ((void**)address)[0] = value;
}

#define TM_READ(X) *(X)

typedef int TM_ABORTED_EXCEPTION;
//#define TM_ABORT() assert(0);

#define TX_SYS_INCONSISTENT_READS false

//////
#elif TX_SYS == TX_SYS_LOCKTABLE

#include "tx_sys/locktable.h"

#define TX_SYS_STATS LOCKTABLE_STATS
typedef struct locktable_stats tx_sys_stats_t;

#define TM_COMMIT_UPDATE locktable_commit_update
#define TM_WRITE locktable_write
#define TM_READ locktable_read

#define tx_sys_get_stats locktable_get_stats

typedef locktable_aborted_exception TM_ABORTED_EXCEPTION;
#define TM_ABORT locktable_abort

#if LOCKTABLE_ALG == LOCKTABLE_SILO
#define TX_SYS_INCONSISTENT_READS true
#elif LOCKTABLE_ALG == LOCKTABLE_LOCKTABLE || LOCKTABLE_ALG == LOCKTABLE_TINYSTM
#define TX_SYS_INCONSISTENT_READS false
#endif

//////
#elif TX_SYS == TX_SYS_FINE_GRAINED_LOCKING

#include "tx_sys/fine_grained_locking.h"

#define TX_SYS_INCONSISTENT_READS false

#define TX_SYS_STATS FGL_STATS
typedef struct fine_grained_locking_stats tx_sys_stats_t;

#define tx_sys_get_stats fgl_get_stats

#define TM_COMMIT_UPDATE(x) {\
        FGL_UNLOCK_ALL(); \
        fgl_get_state()->stats.nCommits++;\
        x = false;\
}


HEADER_INLINE void TM_WRITE(volatile void* address, uint64_t value){
        ((uint64_t*)address)[0] = value;
}

HEADER_INLINE void TM_WRITE(volatile void* address, void* value){
        ((void**)address)[0] = value;
}

#define TM_READ(X) *(X)

typedef int TM_ABORTED_EXCEPTION;

#else
	Assertion error
#endif

