#ifndef SIVFS_COMMON_H
#define SIVFS_COMMON_H

#define dout(fmt, ...)                                  \
        pr_debug(                                       \
                "sivfs: %-30s: %-20s: %-4d: " fmt "\n", \
                __FILE__,                               \
                __PRETTY_FUNCTION__,                    \
                __LINE__,                               \
                ##__VA_ARGS__                           \
        )

#endif
