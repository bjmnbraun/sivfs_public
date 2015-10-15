#ifndef SIVFS_TYPES_H
#define SIVFS_TYPES_H

#include "sivfs_common.h"

typedef __u64 sivfs_ts;

#define SIVFS_INVALID_TS ((sivfs_ts)(-1))

#define _SIVFS_TS_BIT_DIRTY (((sivfs_ts)1) << 63)
HEADER_INLINE bool sivfs_ts_is_dirty(sivfs_ts ts){
        return !!(ts & _SIVFS_TS_BIT_DIRTY);
}
HEADER_INLINE sivfs_ts sivfs_ts_unset_dirty(sivfs_ts ts){
        return ts & ~_SIVFS_TS_BIT_DIRTY;
}
HEADER_INLINE sivfs_ts sivfs_ts_set_dirty(sivfs_ts ts){
        return ts | _SIVFS_TS_BIT_DIRTY;
}

typedef __u64 sivfs_word_t;

#endif
