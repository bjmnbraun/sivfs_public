#ifndef SIVFS_COMMON_H
#define SIVFS_COMMON_H

//Kernel only
#ifdef __KERNEL__

//Needed to get __always_inline in the kernel
#include <linux/compiler.h>
//Needed for size_t
#include <linux/types.h>
//Needed for current->pid
#include <linux/sched.h>

#include <linux/printk.h>

#define dout(fmt, ...)                                  \
        pr_debug(                                       \
                "sivfs: %u %p %-30s: %-20s: %-4d: " fmt "\n", \
                current->pid,                           \
                current,                                \
                __FILE__,                               \
                __PRETTY_FUNCTION__,                    \
                __LINE__,                               \
                ##__VA_ARGS__                           \
        )

#endif

#ifndef HEADER_INLINE
//Used for defining short inline functions in header files.
//Is equivalent to static __always_inline
#define HEADER_INLINE static __always_inline
#endif

//This is a bit yucky, since it requires size_t which may not be available in
//some userspace apps we want to work with.
HEADER_INLINE size_t max3_size_t(
        size_t a,
        size_t b,
        size_t c
){
        if (a >= b && a >= c) return a;
        if (b >= a && b >= c) return b;
        //Remaining condition
        return c;
}

HEADER_INLINE size_t max2_size_t(
        size_t a,
        size_t b
){
        if (a >= b) return a;
        //Remaining condition
        return b;
}

#endif
