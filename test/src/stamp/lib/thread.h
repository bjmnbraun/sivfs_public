/* =============================================================================
 *
 * thread.h
 *
 * =============================================================================
 *
 * Copyright (C) Stanford University, 2006.  All Rights Reserved.
 * Author: Chi Cao Minh
 *
 * =============================================================================
 *
 * For the license of bayes/sort.h and bayes/sort.c, please see the header
 * of the files.
 *
 * ------------------------------------------------------------------------
 *
 * For the license of kmeans, please see kmeans/LICENSE.kmeans
 *
 * ------------------------------------------------------------------------
 *
 * For the license of ssca2, please see ssca2/COPYRIGHT
 *
 * ------------------------------------------------------------------------
 *
 * For the license of lib/mt19937ar.c and lib/mt19937ar.h, please see the
 * header of the files.
 *
 * ------------------------------------------------------------------------
 *
 * For the license of lib/rbtree.h and lib/rbtree.c, please see
 * lib/LEGALNOTICE.rbtree and lib/LICENSE.rbtree
 *
 * ------------------------------------------------------------------------
 *
 * Unless otherwise noted, the following license applies to STAMP files:
 *
 * Copyright (c) 2007, Stanford University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Stanford University nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY STANFORD UNIVERSITY ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL STANFORD UNIVERSITY BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * =============================================================================
 */


#ifndef THREAD_H
#define THREAD_H 1


#include <pthread.h>
#include <stdlib.h>

#include <unistd.h>
#include <linux/unistd.h>
#include <asm/ldt.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "types.h"
#ifdef OTM
#include "omp.h"
#endif

#include "tm.h"


//TODO config for stamp
#define STAMP_LIB_USES_THREADS 1
#define STAMP_LIB_USES_PROCESSES 2
#define STAMP_LIB_THREAD_MODE STAMP_LIB_USES_PROCESSES

#ifdef __cplusplus
extern "C" {
#endif

typedef struct barrier {
  pthread_cond_t complete;
  pthread_mutex_t mutex;
    int count;
  //How many have crossed in this countset
  int crossing;
  //Which countset (we flip this every time the barrier is opened)
  int countset;
} thread_spinbarrier_t;


#define THREAD_T                            pthread_t
#define THREAD_ATTR_T                       pthread_attr_t

#define THREAD_ATTR_INIT(attr)              pthread_attr_init(&attr)
#define THREAD_JOIN(tid)                    pthread_join(tid, (void**)NULL)
#define THREAD_CREATE(tid, attr, fn, arg)   pthread_create(&(tid), \
                                                           &(attr), \
                                                           (void* (*)(void*))(fn), \
                                                           (void*)(arg))

#define THREAD_LOCAL_T                      pthread_key_t
#define THREAD_LOCAL_INIT(key)              pthread_key_create(&key, NULL)
#define THREAD_LOCAL_SET(key, val)          pthread_setspecific(key, (void*)(val))
#define THREAD_LOCAL_GET(key)               pthread_getspecific(key)

#define THREAD_MUTEX_T                      pthread_mutex_t
#define THREAD_MUTEX_INIT(lock)             pthread_mutex_init(&(lock), NULL)
#define THREAD_MUTEX_LOCK(lock)             pthread_mutex_lock(&(lock))
#define THREAD_MUTEX_UNLOCK(lock)           pthread_mutex_unlock(&(lock))

#define THREAD_COND_T                       pthread_cond_t
#define THREAD_COND_INIT(cond)              pthread_cond_init(&(cond), NULL)
#define THREAD_COND_SIGNAL(cond)            pthread_cond_signal(&(cond))
#define THREAD_COND_BROADCAST(cond)         pthread_cond_broadcast(&(cond))
#define THREAD_COND_WAIT(cond, lock)        pthread_cond_wait(&(cond), &(lock))

/* Follow the original stamp behaviour */
#ifdef SIMULATOR
#  define BARRIER_PTHREAD
#endif

/* Default BARRIER */
/* Original STAMP had BARRIER_LOGARITHMIC but pthread_barrier seems pretty good. */
#if !defined(BARRIER_PTHREAD) && !defined(BARRIER_LOGARITHMIC) && !defined(BARRIER_SPINNING)
#  define BARRIER_PTHREAD
#endif

#ifdef BARRIER_PTHREAD
#  define THREAD_BARRIER_T                  pthread_barrier_t
#  define THREAD_BARRIER_ALLOC(N)           ((THREAD_BARRIER_T*)malloc(sizeof(THREAD_BARRIER_T)))
#  define THREAD_BARRIER_INIT(bar, N)       pthread_barrier_init(bar, 0, N)
#  define THREAD_BARRIER(bar, tid)          pthread_barrier_wait(bar); 

#  define THREAD_BARRIER_FREE(bar)          free(bar)
#endif

#ifdef BARRIER_LOGARITHMIC
#  define THREAD_BARRIER_T                  thread_barrier_t
#  define THREAD_BARRIER_ALLOC(N)           thread_barrier_alloc(N)
#  define THREAD_BARRIER_INIT(bar, N)       thread_barrier_init(bar)
#  define THREAD_BARRIER(bar, tid)          thread_barrier(bar, tid)
#  define THREAD_BARRIER_FREE(bar)          thread_barrier_free(bar)
#endif

/* Spin barrier is useful with simulation. It avoids to go to kernel space. */
#ifdef BARRIER_SPINNING
#  define THREAD_BARRIER_T                  thread_spinbarrier_t
#  define THREAD_BARRIER_ALLOC(N)           thread_spinbarrier_alloc() //N
#  define THREAD_BARRIER_INIT(bar, N)       thread_spinbarrier_init(bar, N)
#  define THREAD_BARRIER(bar, tid)          thread_spinbarrier(bar) //,tid
#  define THREAD_BARRIER_FREE(bar)          thread_spinbarrier_free(bar)
#endif

#if STAMP_LIB_THREAD_MODE == STAMP_LIB_USES_PROCESSES
//Override some of the init routines above to set shared where necessary
//and to allocate from shared memory

#undef THREAD_T
#define THREAD_T                            int
#undef THREAD_ATTR_T
#define THREAD_ATTR_T                       int

#undef THREAD_ATTR_INIT
#define THREAD_ATTR_INIT(attr)              (0)
#undef THREAD_JOIN
#define THREAD_JOIN(tid)                    waitpid(tid, NULL, 0)

HEADER_INLINE int _THREAD_CREATE(
        int* tid,
        int* attr,
        void* (*fn)(void*),
        void* arg
){
        int child = fork();
        if (child == 0){
                //thread local storage is not reset on fork, 
                //so can't rely on that here.
                void* ignored = fn(arg);
                exit(0);
        }
        if (child == -1){
                return -EINVAL;
        }
        *tid = child;
        return 0;
}

#undef THREAD_CREATE
#define THREAD_CREATE(tid, attr, fn, arg) _THREAD_CREATE( \
        &(tid), \
        &(attr), \
        (void* (*)(void*))(fn), \
        (void*)(arg) \
)

//TODO mutexes need to be allocated from shared memory!
HEADER_INLINE int _THREAD_MUTEX_INIT(THREAD_MUTEX_T * mutex){
        int rc = 0;
        pthread_mutexattr_t attr;
        rc = pthread_mutexattr_init(&attr);
        if (rc) return rc;
        rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc) return rc;
        rc = pthread_mutex_init(mutex, &attr);
        return rc;
}

#undef THREAD_MUTEX_INIT
#define THREAD_MUTEX_INIT(lock)             _THREAD_MUTEX_INIT(&(lock))

HEADER_INLINE int _THREAD_COND_INIT(THREAD_COND_T * cond){
        int rc = 0;
        pthread_condattr_t attr;
        rc = pthread_condattr_init(&attr);
        if (rc) return rc;
        rc = pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc) return rc;
        rc = pthread_cond_init(cond, &attr);
        return rc;
}
#undef THREAD_COND_INIT
#define THREAD_COND_INIT(cond)             _THREAD_COND_INIT(&(cond))

#undef THREAD_BARRIER_ALLOC
HEADER_INLINE THREAD_BARRIER_T* THREAD_BARRIER_ALLOC(size_t N){
        THREAD_BARRIER_T* bar = NULL;

        //Super wasteful, but works, shared memory allocation:
        bar = (THREAD_BARRIER_T*) mmap(
                NULL,
                4096,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS,
                -1,
                0
        );
        if (!bar){ assert(0); return NULL; }

        //Currently only works with BARRIER_PTHREAD options above
        int rc = 0;
        pthread_barrierattr_t attr;
        rc = pthread_barrierattr_init(&attr);
        if (rc) { assert(0); return NULL; }
        rc = pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc) { assert(0); return NULL; }
        rc = pthread_barrier_init(bar, &attr, N);
        if (rc) { assert(0); return NULL; }

        return bar;
}

#undef THREAD_BARRIER_FREE
#  define THREAD_BARRIER_FREE(bar)          (0)

#undef THREAD_BARRIER_INIT
#define THREAD_BARRIER_INIT(bar, N)       (0)

#endif


/* =============================================================================
 * thread_startup
 * -- Create pool of secondary threads
 * -- numThread is total number of threads (primary + secondary)
 * =============================================================================
 */
void
thread_startup (long numThread);


/* =============================================================================
 * thread_start
 * -- Make primary and secondary threads execute work
 * -- Should only be called by primary thread
 * -- funcPtr takes one arguments: argPtr
 * =============================================================================
 */
void
thread_start (void (*funcPtr)(void*), void* argPtr);


/* =============================================================================
 * thread_shutdown
 * -- Primary thread kills pool of secondary threads
 * =============================================================================
 */
void
thread_shutdown ();

typedef struct thread_barrier {
    THREAD_MUTEX_T countLock;
    THREAD_COND_T proceedCond;
    THREAD_COND_T proceedAllCond;
    long count;
    long numThread;
} thread_barrier_t;

  thread_spinbarrier_t *thread_spinbarrier_alloc() ;
  void thread_spinbarrier_free(thread_spinbarrier_t *b);
  void thread_spinbarrier_init(thread_spinbarrier_t *b, int n);
  void thread_spinbarrier(thread_spinbarrier_t *b);


/* =============================================================================
 * thread_barrier_alloc
 * =============================================================================
 */
thread_barrier_t*
thread_barrier_alloc (long numThreads);


/* =============================================================================
 * thread_barrier_free
 * =============================================================================
 */
void
thread_barrier_free (thread_barrier_t* barrierPtr);


/* =============================================================================
 * thread_barrier_init
 * =============================================================================
 */
void
thread_barrier_init (thread_barrier_t* barrierPtr);


/* =============================================================================
 * thread_barrier
 * -- Simple logarithmic barrier
 * =============================================================================
 */
void
thread_barrier (thread_barrier_t* barrierPtr, long threadId);

/* =============================================================================
 * thread_getId
 * -- Call after thread_start() to get thread ID inside parallel region
 * =============================================================================
 */
long
thread_getId();


/* =============================================================================
 * thread_getNumThread
 * -- Call after thread_start() to get number of threads inside parallel region
 * =============================================================================
 */
long
thread_getNumThread();

/* =============================================================================
 * thread_barrier_wait
 * -- Call after thread_start() to synchronize threads inside parallel region
 * =============================================================================
 */
void
thread_barrier_wait();

#ifdef __cplusplus
}
#endif


#endif /* THREAD_H */


/* =============================================================================
 *
 * End of thread.h
 *
 * =============================================================================
 */
