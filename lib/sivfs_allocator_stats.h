#ifndef SIVFS_ALLOCATOR_STATS_H
#define SIVFS_ALLOCATOR_STATS_H

// Allocs / frees are successful but possibly not committed
// Committed* are committed allocs / frees.
#define SIVFS_ALLOCATOR_STATS \
        F(nAllocs) \
        F(nFrees) \
        F(nCommittedAllocs) \
        F(nCommittedFrees) \

struct sivfs_allocator_stats {
#define F(X) size_t X;
        SIVFS_ALLOCATOR_STATS
#undef F
};

#endif
