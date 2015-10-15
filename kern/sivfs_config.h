#ifndef SIVFS_CONFIG_H
#define SIVFS_CONFIG_H

#ifdef __KERNEL__

//Kernel only config

#endif

#define SIVFS_COMPAT_LINUX_4_2_1 1
#define SIVFS_COMPAT_LINUX_4_6 (SIVFS_COMPAT_LINUX_4_2_1 + 1)
#define SIVFS_COMPAT_LINUX_4_8 (SIVFS_COMPAT_LINUX_4_6 + 1)
#define SIVFS_COMPAT_LINUX_4_9 (SIVFS_COMPAT_LINUX_4_8 + 1)
#define SIVFS_COMPAT_LINUX_4_10 (SIVFS_COMPAT_LINUX_4_9 + 1)
#define SIVFS_COMPAT_LINUX_NEWEST (SIVFS_COMPAT_LINUX_4_10 + 1)

#define SIVFS_COMPAT_LINUX SIVFS_COMPAT_LINUX_NEWEST

//Static configuration of sivfs
//Mostly for benchmarking differente behavior and their performance effects

#define SIVFS_DEFAULT_MODE      0755
#define SIVFS_MAGIC             0x203984

#define MAX_COMMIT_SIZE 4096*64

//Checkpoint handling
#define SIVFS_CHECKPOINT_INO_LEVELS 3
//Maxes out at 5
#define SIVFS_CHECKPOINT_FOFFSET_LEVELS 4
//Delay between snapshots in microseconds. Actual delay between [Min, Min+Delta]
#define SIVFS_CHECKPOINT_UDELAY_MIN 100
#define SIVFS_CHECKPOINT_UDELAY_DELTA 30
#define SIVFS_MIN_COMMITS_PER_CHECKPOINT 1
#define SIVFS_MAX_WRITE_TXNS_SINCE_CHECKPOINT 1000

//Maximum memory usage due to checkpoint pages.
//TODO we are currently reliant on this, if you set this too high and
//use more memory than you have physical memory the system will hang. I am
//looking into a more elegant way to avoid this outcome, but a temporary
//workaround is setting this relatively small.
#define SIVFS_MAX_CHECKPOINT_PAGES_MB 4000

//For sequential log, how large to allocate chunks (min)
#define SIVFS_LOG_CHUNK_SIZE 1024

//Delay between snapshot gc calls in milliseconds (slept)
#define SIVFS_CHECKPOINT_GC_MDELAY 10
//Delay between log gc calls in milliseconds (slept)
#define SIVFS_LOG_GC_MDELAY 10

//Debugging flags
#define SIVFS_DEBUG_STATS	     false
#define SIVFS_DEBUG_GC               false
#define SIVFS_DEBUG_GC_FREES         false
#define SIVFS_DEBUG_MAKE_CHECKPOINTS false
#define SIVFS_DEBUG_COMMIT           false
#define SIVFS_DEBUG_UPDATE           false
#define SIVFS_DEBUG_PGFAULTS         false
#define SIVFS_DEBUG_INODES           false
#define SIVFS_DEBUG_THREADS          true
#define SIVFS_DEBUG_PGINFO           true
#define SIVFS_DEBUG_MMAPS            true
#define SIVFS_DEBUG_ABORTS           false
#define SIVFS_DEBUG_MMU_NOTIFIER     false
#define SIVFS_DEBUG_ALLOC	     false
#define SIVFS_DEBUG_DIRECT_ACCESS    false

//FIGURES
#define SIVFS_FIGURE_SEQLOG_NONSEQ 1
#define SIVFS_FIGURE_SEQLOG_SEQ (SIVFS_FIGURE_SEQLOG_NONSEQ + 1)

#define SIVFS_FIGURE_SEQLOG_MODE SIVFS_FIGURE_SEQLOG_SEQ
//#define SIVFS_FIGURE_SEQLOG_MODE SIVFS_FIGURE_SEQLOG_NONSEQ

#define SIVFS_FIGURE_NO_CHECKOUT_CP_PAGES 1
#define SIVFS_FIGURE_YES_CHECKOUT_CP_PAGES (SIVFS_FIGURE_NO_CHECKOUT_CP_PAGES + 1)

#define SIVFS_FIGURE_CHECKOUT_CP_PAGES SIVFS_FIGURE_YES_CHECKOUT_CP_PAGES

#define SIVFS_FIGURE_NO_CHECKOUT_CPS 1
#define SIVFS_FIGURE_YES_CHECKOUT_CPS (SIVFS_FIGURE_NO_CHECKOUT_CPS + 1)

//CHECKING OUT CPS NOT YET WORKING - DO NOT ENABLE
//#define SIVFS_FIGURE_CHECKOUT_CPS SIVFS_FIGURE_YES_CHECKOUT_CPS
#define SIVFS_FIGURE_CHECKOUT_CPS SIVFS_FIGURE_NO_CHECKOUT_CPS

//Use session- or batch-based external consistency approach to allow us
//to serve stale reads safely. Should be a dramatic speedup for some benchmarks
//
//Currently only "batch" is implemented, and is the default behavior.
#define SIVFS_FIGURE_NO_EXTERNAL_CONSISTENCY_TRICKS 1
#define SIVFS_FIGURE_YES_EXTERNAL_CONSISTENCY_TRICKS 2

#define SIVFS_FIGURE_EXTERNAL_CONSISTENCY_TRICKS SIVFS_FIGURE_NO_EXTERNAL_CONSISTENCY_TRICKS

/*
#define CONFLICT_DETECTION_VALUE_BASED
#define CONFLICT_DETECTION_ADDRESS_BASED
*/

//Add 1 just so that 0 is not a valid value. Set to 1 for no workingset.
//Set to -1 for unbounded workingset.
//
//Note that checked out checkout pages are not considered part of the
//workingset.
//#define SIVFS_FIGURE_WORKINGSET_HARDLIMIT_PLUS_ONE 1
#define SIVFS_FIGURE_WORKINGSET_HARDLIMIT_PLUS_ONE -1

//TODO this is possibly better as a mount option for the filesystem?
#define SIVFS_MAX_CHECKPOINT_PAGES_BYTES (\
        (size_t)(SIVFS_MAX_CHECKPOINT_PAGES_MB) * (1ULL<<20)\
)

//Local overrides. File must be created before building, but empty is OK.
#include "sivfs_config_local.h"

#ifdef __KERNEL__
//Doesn't need to ever be called. Putting the static assert inside a
//made up function here prevents smatch from failing to parse the assert
HEADER_INLINE void sivfs_validate_config(void) {
        _Static_assert(
                SIVFS_CHECKPOINT_INO_LEVELS > 0,
                "Must have at least one ino level"
        );
        _Static_assert(
                SIVFS_CHECKPOINT_INO_LEVELS <= 3,
                "Too many ino levels"
        );
        _Static_assert(
                SIVFS_CHECKPOINT_FOFFSET_LEVELS > 0,
                "Must have at least one foffset level"
        );
        _Static_assert(
                SIVFS_CHECKPOINT_FOFFSET_LEVELS <= 5,
                "Too many foffset levels"
        );

        _Static_assert(
                SIVFS_MIN_COMMITS_PER_CHECKPOINT > 0,
                "A snapshot can happen as frequently as one per commit"
        );
}
#endif

//Convenience macros:
#define SIVFS_TRACK_CLEAR_WORKINGSET_EVERY_TXN (SIVFS_FIGURE_WORKINGSET_HARDLIMIT_PLUS_ONE == 1)

#define SIVFS_MAX_CHECKPOINT_PAGES (\
        SIVFS_MAX_CHECKPOINT_PAGES_BYTES / PAGE_SIZE\
)

#endif
