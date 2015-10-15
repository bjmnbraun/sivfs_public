#pragma once

#define TX_SYS_SIVFS 1
#define TX_SYS_GL (TX_SYS_SIVFS + 1)
#define TX_SYS_LOCKTABLE (TX_SYS_GL + 1)
//Must be handwritten for each benchmark
#define TX_SYS_FINE_GRAINED_LOCKING (TX_SYS_LOCKTABLE + 1)

#define TX_SYS TX_SYS_VALUE(TX_SYS_SIVFS)

#define GL_SPIN 1
#define GL_MUTEX (GL_SPIN + 1)

#define GL_MUTEX_TYPE TX_SYS_VALUE(GL_SPIN)

//Number of entries in locktable txsys
#define LOCKTABLE_SIZE TX_SYS_VALUE(1024)

#define LOCKTABLE_LOCKTABLE 1
#define LOCKTABLE_TINYSTM (LOCKTABLE_LOCKTABLE + 1)
#define LOCKTABLE_SILO (LOCKTABLE_TINYSTM + 1)

//Which algorithm to use for locktable
#define LOCKTABLE_ALG TX_SYS_VALUE(LOCKTABLE_LOCKTABLE)

#define TX_SYS_ARGS \
        F(TX_SYS) \
        F(GL_MUTEX_TYPE) \
        F(LOCKTABLE_SIZE) \
        F(LOCKTABLE_ALG) \

//Default TX_SYS_VALUE to identity
#define TX_SYS_VALUE(X) X

void txsys_compile_args_print(unsigned long run_uid);

#include "txsys_config_local.h"
