#ifndef SIVFS_STATS_H
#define SIVFS_STATS_H

#include "sivfs_common.h"

#define SIVFS_STAT(x) F(x)

#define SIVFS_STATS \
        SIVFS_STAT(nTxns) \
        SIVFS_STAT(nCommits) \
        SIVFS_STAT(nPgFaults) \
        SIVFS_STAT(nWriteTxns) \
        SIVFS_STAT(nWriteTxnsWrites) \
        SIVFS_STAT(nCheckpoints) \
        SIVFS_STAT(nCheckpointsTxns) \
        SIVFS_STAT(nWriteTxnsThrottledOnSS) \
        SIVFS_STAT(nCoWCPPagesMkwrite) \
        SIVFS_STAT(nCoWCPPagesInitialWrite) \
        SIVFS_STAT(nCheckoutCPPages) \
        SIVFS_STAT(nCheckpointPagesNew) \
        SIVFS_STAT(nCheckpointPagesFreed) \
        SIVFS_STAT(nCheckpointInodesFreed) \
        SIVFS_STAT(nCheckedOutCheckpoints) \

struct sivfs_stats {
#define F(x) unsigned long long x;
        SIVFS_STATS
#undef F
};

//
void sivfs_update_stats(void);

#endif
