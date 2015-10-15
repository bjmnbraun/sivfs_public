/* =============================================================================
 *
 * thread.c
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


#include <assert.h>
#include <stdlib.h>
#include "tm.h"
#include "thread.h"
#include "types.h"


#ifdef __cplusplus
extern "C" {
#endif

static THREAD_LOCAL_T    global_threadId;

struct thread_shared_state {
        long              numThread       = 1;
        long*             threadIds       = NULL;
        THREAD_ATTR_T     threadAttr;
        void              (*funcPtr)(void*) = NULL;
        void*             argPtr          = NULL;
        volatile bool_t   doShutdown      = FALSE;
        THREAD_BARRIER_T* barrierPtr      = NULL;
        THREAD_T*         threads         = NULL;
};

static struct thread_shared_state* global_shared_state;

static int
init_global_shared_state(long numThread){
        int rc = 0;

        //Super wasteful, but works, shared memory allocation:
        global_shared_state = (struct thread_shared_state*) mmap(
                        NULL,
                        4096,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS,
                        -1,
                        0
        );
        if (!global_shared_state){
                assert(0);
                rc = -EINVAL;
                goto error0;
        }

        //malocs below need to be mmaps too
    global_shared_state->numThread = numThread;
    global_shared_state->doShutdown = FALSE;

    /* Set up barrier */
    assert(global_shared_state->barrierPtr == NULL);
    global_shared_state->barrierPtr = THREAD_BARRIER_ALLOC(numThread);
    assert(global_shared_state->barrierPtr);
    THREAD_BARRIER_INIT(global_shared_state->barrierPtr, numThread);

    /* Set up ids */
    THREAD_LOCAL_INIT(global_threadId);
    assert(global_shared_state->threadIds == NULL);
    global_shared_state->threadIds = (long*)malloc(numThread * sizeof(long));
    assert(global_shared_state->threadIds);
    long i;
    for (i = 0; i < numThread; i++) {
        global_shared_state->threadIds[i] = i;
    }

    /* Set up thread list */
    //TODO do inside init_global_shared_state->shared_state
    assert(global_shared_state->threads == NULL);
    global_shared_state->threads = (THREAD_T*)malloc(numThread * sizeof(THREAD_T));
    assert(global_shared_state->threads);
    
    THREAD_ATTR_INIT(global_shared_state->threadAttr);

error0:
        return rc;
}

/* =============================================================================
 * threadWait
 * -- Synchronizes all threads to start/stop parallel section
 * =============================================================================
 */
static void
threadWait (void* argPtr)
{
    long threadId = *(long*)argPtr;
  
    THREAD_LOCAL_SET(global_threadId, (long)threadId);

    while (1) {
        THREAD_BARRIER(global_shared_state->barrierPtr, threadId); /* wait for start parallel */
        if (global_shared_state->doShutdown) {
            break;
        }

	
        global_shared_state->funcPtr(global_shared_state->argPtr);
	
        THREAD_BARRIER(global_shared_state->barrierPtr, threadId); /* wait for end parallel */
        if (threadId == 0) {
            break;
        }
    }
}


/* =============================================================================
 * thread_startup
 * -- Create pool of secondary threads
 * -- numThread is total number of threads (primary + secondaries)
 * =============================================================================
 */
void
thread_startup (long numThread)
{
    long i;

    //   TM_STARTUP();
    //TM_THREAD_ENTER();

    int rc = 0;
    rc = init_global_shared_state(numThread);
    assert(rc == 0);

    /* Set up pool */
    for (i = 1; i < numThread; i++) {
        THREAD_CREATE(global_shared_state->threads[i],
                      global_shared_state->threadAttr,
                      &threadWait,
                      &global_shared_state->threadIds[i]);

    }

    /*
     * Wait for primary thread to call thread_start
     */

    std::string child_pids;
    for(i = 1; i < numThread; i++){
        child_pids += std::to_string(global_shared_state->threads[i]);
        child_pids += " ";
    }

    printf(
                    "\nTo debug break thread_startup:wake_up_workers then "
                    " attach gdb instances to children pids: %s\n",
                    child_pids.c_str()
    );

wake_up_workers:
    return; 
}


/* =============================================================================
 * thread_start
 * -- Make primary and secondary threads execute work
 * -- Should only be called by primary thread
 * -- funcPtr takes one arguments: argPtr
 * =============================================================================
 */
void
thread_start (void (*funcPtr)(void*), void* argPtr)
{
    global_shared_state->funcPtr = funcPtr;
    global_shared_state->argPtr = argPtr;

    long threadId = 0; /* primary */
    threadWait((void*)&threadId);
}


/* =============================================================================
 * thread_shutdown
 * -- Primary thread kills pool of secondary threads
 * =============================================================================
 */
void
thread_shutdown ()
{
  printf("thread shutdown\n");
    /* Make secondary threads exit wait() */
    global_shared_state->doShutdown = TRUE;
    THREAD_BARRIER(global_shared_state->barrierPtr, 0);

    long numThread = global_shared_state->numThread;

    long i;
    for (i = 1; i < numThread; i++) {
        THREAD_JOIN(global_shared_state->threads[i]);
    }

    THREAD_BARRIER_FREE(global_shared_state->barrierPtr);

    //No need for this...
    global_shared_state->barrierPtr = NULL;

    //free(global_threadIds);
    global_shared_state->threadIds = NULL;

    //free(global_threads);
    global_shared_state->threads = NULL;

    global_shared_state->numThread = 1;

    TM_SHUTDOWN();

}

  /*=== 
   * spin barrier
   */
void thread_spinlock_acquire(/*uint64_t**/pthread_mutex_t* lock){
  //printf("lock acquhre\n");
  while (__sync_lock_test_and_set((uint64_t*)lock, 1) != 0)
    {
      uint64_t val;
      do {
	//	_mm_pause();
	val = __sync_val_compare_and_swap((uint64_t*)lock, 1, 1);
      } while (val == 1);
    }
}


void thread_spinlock_release(/*uint64_t**/ pthread_mutex_t* lock){
  //printf("lock release\n");
  __sync_lock_release((uint64_t*)lock);
}



thread_spinbarrier_t *thread_spinbarrier_alloc() {
    return (thread_spinbarrier_t *)malloc(sizeof(thread_spinbarrier_t));
}

void thread_spinbarrier_free(thread_spinbarrier_t *b) {
    free(b);
}

void thread_spinbarrier_init(thread_spinbarrier_t *b, int n) {
    pthread_cond_init(&b->complete, NULL);
    pthread_mutex_init(&b->mutex, NULL);
    b->count = n;
    b->crossing = 0;
    b->countset = 0;
}

void thread_spinbarrier(thread_spinbarrier_t *b) {
  thread_spinlock_acquire(&b->mutex);
  //    pthread_mutex_lock(&b->mutex);
/* One more thread through */
    b->crossing++;
    /* If not all here, wait */
    int old_countset = b->countset;
    if (b->crossing < b->count){
      while (b->countset == old_countset) {
	thread_spinlock_release(&b->mutex);
	//pthread_mutex_unlock(&b->mutex);
	thread_spinlock_acquire(&b->mutex);
	//pthread_mutex_lock(&b->mutex);
      }
    } else {
      //We are the last thread, flip to the next countset
      b->countset = !b->countset;
      b->crossing = 0;
    }
    thread_spinlock_release(&b->mutex);
    //    pthread_mutex_unlock(&b->mutex);

    //   if (b->crossing < b->count) {
    //    pthread_cond_wait(&b->complete, &b->mutex);
    //} else {
        /* Reset for next time */
    //   b->crossing = 0;
        //pthread_cond_broadcast(&b->complete);
	//}
    
    //pthread_mutex_unlock(&b->mutex);
}
  //END spin barrier


/* =============================================================================
 * thread_barrier_alloc
 * =============================================================================
 */
thread_barrier_t*
thread_barrier_alloc (long numThread)
{
    thread_barrier_t* barrierPtr;

    assert(numThread > 0);
    assert((numThread & (numThread - 1)) == 0); /* must be power of 2 */
    barrierPtr = (thread_barrier_t*)malloc(numThread * sizeof(thread_barrier_t));
    if (barrierPtr != NULL) {
        barrierPtr->numThread = numThread;
    }

    return barrierPtr;
}


/* =============================================================================
 * thread_barrier_free
 * =============================================================================
 */
void
thread_barrier_free (thread_barrier_t* barrierPtr)
{
    free(barrierPtr);
}


/* =============================================================================
 * thread_barrier_init
 * =============================================================================
 */
void
thread_barrier_init (thread_barrier_t* barrierPtr)
{
    long i;
    long numThread = barrierPtr->numThread;

    for (i = 0; i < numThread; i++) {
        barrierPtr[i].count = 0;
        THREAD_MUTEX_INIT(barrierPtr[i].countLock);
        THREAD_COND_INIT(barrierPtr[i].proceedCond);
        THREAD_COND_INIT(barrierPtr[i].proceedAllCond);
    }
}


/* =============================================================================
 * thread_barrier
 * -- Simple logarithmic barrier
 * =============================================================================
 */
void
thread_barrier (thread_barrier_t* barrierPtr, long threadId)
{
    long i = 2;
    long base = 0;
    long index;
    long numThread = barrierPtr->numThread;

    if (numThread < 2) {
        return;
    }

    do {
        index = base + threadId / i;
        if ((threadId % i) == 0) {
            THREAD_MUTEX_LOCK(barrierPtr[index].countLock);
            barrierPtr[index].count++;
            while (barrierPtr[index].count < 2) {
                THREAD_COND_WAIT(barrierPtr[index].proceedCond,
                                 barrierPtr[index].countLock);
            }
            THREAD_MUTEX_UNLOCK(barrierPtr[index].countLock);
        } else {
            THREAD_MUTEX_LOCK(barrierPtr[index].countLock);
            barrierPtr[index].count++;
            if (barrierPtr[index].count == 2) {
                THREAD_COND_SIGNAL(barrierPtr[index].proceedCond);
            }
            while (THREAD_COND_WAIT(barrierPtr[index].proceedAllCond,
                                    barrierPtr[index].countLock) != 0)
            {
                /* wait */
            }
            THREAD_MUTEX_UNLOCK(barrierPtr[index].countLock);
            break;
        }
        base = base + numThread / i;
        i *= 2;
    } while (i <= numThread);

    for (i /= 2; i > 1; i /= 2) {
        base = base - numThread / i;
        index = base + threadId / i;
        THREAD_MUTEX_LOCK(barrierPtr[index].countLock);
        barrierPtr[index].count = 0;
        THREAD_COND_SIGNAL(barrierPtr[index].proceedAllCond);
        THREAD_MUTEX_UNLOCK(barrierPtr[index].countLock);
    }
}


/* =============================================================================
 * thread_getId
 * -- Call after thread_start() to get thread ID inside parallel region
 * =============================================================================
 */
long
thread_getId()
{
    return (long)THREAD_LOCAL_GET(global_threadId);
}


/* =============================================================================
 * thread_getNumThread
 * -- Call after thread_start() to get number of threads inside parallel region
 * =============================================================================
 */
long
thread_getNumThread()
{
    return global_shared_state->numThread;
}


/* =============================================================================
 * thread_barrier_wait
 * -- Call after thread_start() to synchronize threads inside parallel region
 * =============================================================================
 */
void
thread_barrier_wait()
{
  TM_TXN_PAUSE();
    THREAD_BARRIER(global_shared_state->barrierPtr, threadId);
  //Do a null txn to check out a fresh snapshot
        bool aborted;
        TM_COMMIT_UPDATE(aborted);
        assert(!aborted);
}


/* =============================================================================
 * TEST_THREAD
 * =============================================================================
 */
#ifdef TEST_THREAD


#include <stdio.h>
#include <unistd.h>


#define NUM_THREADS    (4)
#define NUM_ITERATIONS (3)



void
printId (void* argPtr)
{
    long threadId = thread_getId();
    long numThread = thread_getNumThread();
    long i;

    for ( i = 0; i < NUM_ITERATIONS; i++ ) {
        thread_barrier_wait();
        if (threadId == 0) {
            sleep(1);
        } else if (threadId == numThread-1) {
            usleep(100);
        }
        printf("i = %li, tid = %li\n", i, threadId);
        if (threadId == 0) {
            puts("");
        }
        fflush(stdout);
    }
}


int
main ()
{
    puts("Starting...");

    /* Run in parallel */
    thread_startup(NUM_THREADS);
    /* Start timing here */
    thread_start(printId, NULL);
    thread_start(printId, NULL);
    thread_start(printId, NULL);
    /* Stop timing here */
    thread_shutdown();

    puts("Done.");

    return 0;
}


#endif /* TEST_THREAD */

#ifdef __cplusplus
}
#endif

/* =============================================================================
 *
 * End of thread.c
 *
 * =============================================================================
 */
