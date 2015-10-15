#pragma once

#include <pthread.h>
#include <cassert>
#include <atomic>

#include "common.h"

//Multi-process safe concurrency primitives
class simple_spinlock {
  public:
        void lock(){
                bool expected = false;
                while(!locked.compare_exchange_weak(expected, true)){
                        expected = false;
                        cpu_relax();
                }
        }

        void unlock(){
                locked.store(false, std::memory_order_relaxed);
        }

  private:
        std::atomic<bool> locked;
};

class simple_mutex {
  public:
        simple_mutex(){
                int rc = 0;

                pthread_mutexattr_t attr;
                rc = pthread_mutexattr_init(&attr);
                assert(rc == 0);

                rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
                assert(rc == 0);

                rc = pthread_mutex_init(&mutex, &attr);
                assert(rc == 0);

        }

        void lock(){
                int rc = pthread_mutex_lock(&mutex);
                assert(rc == 0);
        }

        void unlock(){
                int rc = pthread_mutex_unlock(&mutex);
                assert(rc == 0);
        }
  private:
        pthread_mutex_t mutex;
};
