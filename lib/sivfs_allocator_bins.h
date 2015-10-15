#ifndef SIVFS_ALLOCATOR_BINS
#define SIVFS_ALLOCATOR_BINS

#include <math.h>

//Needed for offsetof
#include "stddef.h"
#include "assert.h"
#include "sivfs.h"
#include "sivfs_allocator_stats.h"
#include "sivfs_allocator_common.h"
#include "sivfs_allocator_list.h"

//TODO config file
#define SIVFS_ALLOCATOR_NBINS 51

//Each bin just contains a pointer to head of a free list
//or null if the bin is empty
//i.e. zero initialization is ok
struct sivfs_allocator_bin {
        struct sivfs_allocator_lh* free_list;
};

struct sivfs_allocator_bins {
        sivfs_allocator_bin bins[SIVFS_ALLOCATOR_NBINS];
};

//Exclusive upper bound on binsize.
HEADER_INLINE size_t sivfs_allocator_bin_maxsize(
        size_t binindex
){
        return floor(exp((binindex+1)*log(1.15)))*sizeof(sivfs_word_t);
}

//Computes the bin size and returns a reference to the appropriate bin
//for an allocation request of size
//
//For large size, bin_out is NULL, bin_size_out is undefined, and the
//allocation should be done as a single chunk. There is guaranteed to be a bin
//for all requests up to PAGE SIZE.
HEADER_INLINE int sivfs_allocator_bins_get_bin(
        size_t* binsize_out,
        struct sivfs_allocator_bin** bin_out,
        struct sivfs_allocator_bins* bins,
        size_t size
){
        int rc = 0;
        size_t binindex, binsize;

        //size must be word-aligned
        if (size % sizeof(sivfs_word_t)){
                rc = -EINVAL;
                goto error0;
        }

        //Mapping from size to bin index
        binindex = floor(log(size/sizeof(sivfs_word_t))/log(1.15));
        if (binindex >= SIVFS_ALLOCATOR_NBINS){
                *bin_out = NULL;
                goto out;
        }
        binsize = sivfs_allocator_bin_maxsize(binindex);
        if (binsize < size){
                //Huh, assertion error.
                //TODO can floating point error cause this?
                rc = -EINVAL;
                goto error0;
        }
        *binsize_out = binsize;
        *bin_out = bins->bins + binindex;

out:
error0:
        return rc;
}

#endif
