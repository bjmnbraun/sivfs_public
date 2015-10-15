#ifndef SIVFS_ALLOCATOR_COMMON
#define SIVFS_ALLOCATOR_COMMON

//Yuck, debug:
#include <stdio.h>
#define uout(fmt, ...)                                  \
        fprintf(stderr,                                 \
                "sivfs_allocator: %-30s: %-20s: %-4d: " fmt "\n", \
                __FILE__,                               \
                __PRETTY_FUNCTION__,                    \
                __LINE__,                               \
                ##__VA_ARGS__                           \
        )

#endif
