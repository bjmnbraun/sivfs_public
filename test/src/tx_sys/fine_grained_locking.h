#pragma once

#include <vector>
#include <mutex>
#include <algorithm>

#include "txsys_config.h"

#include "common.h"
#include "simple_mutex.h"
#include "simple_allocator.h"

//Helpers for writing fine grained locking code
//Assumes code is written to be deadlock free

struct fine_grained_locking_stats {
        unsigned long nCommits;
        //Number of times lock performed
        unsigned long nLocks;
};

#define FGL_STATS \
        F(nCommits) \
        F(nLocks)

typedef simple_spinlock fine_grained_lock_t;

typedef std::lock_guard<simple_spinlock> fine_grained_lock_guard;


struct fine_grained_locking_state {
        struct fine_grained_locking_stats stats;
        struct fine_grained_locking_stats stats_last_snapshot;
        std::vector<fine_grained_lock_t*> lock_vec;
};

extern thread_local struct fine_grained_locking_state* fgl_state;
extern struct fine_grained_locking_stats* fgl_stats;

//fgl_lock must be held to access any of the below
extern simple_mutex* fgl_lock;
extern vector_simple_allocator<struct fine_grained_locking_state*>* fgl_states;
extern _simple_allocator* fgl_allocator;

HEADER_INLINE void fgl_init(/*shared*/ void* shared_space, size_t size_shared_space){
        _simple_allocator* allocator;
        if (size_shared_space < sizeof(*allocator)){
                assert(0);
        }
        allocator = (decltype(allocator))shared_space;

        shared_space = (char*)shared_space + sizeof(*allocator);
        size_shared_space -= sizeof(*allocator);
        new (allocator) _simple_allocator(shared_space, size_shared_space);
        fgl_allocator = allocator;

        simple_allocator_allocate(fgl_lock, allocator);
        new (fgl_lock) simple_mutex();

        simple_allocator_allocate(fgl_states, allocator);
        new (fgl_states) vector_simple_allocator<
                struct fine_grained_locking_state*
        >(allocator);

        simple_allocator_allocate(fgl_stats, allocator);
        new (fgl_stats) struct fine_grained_locking_stats({});
}

HEADER_INLINE void _fgl_update_stats(struct fine_grained_locking_stats* stats,
struct fine_grained_locking_state* state){
        struct fine_grained_locking_stats stats_snapshot = state->stats;

#define F(X) \
        stats->X += stats_snapshot.X - state->stats_last_snapshot.X;

        FGL_STATS

#undef F
        //Reset snapshot
        state->stats_last_snapshot = stats_snapshot;
}

HEADER_INLINE void fgl_update_stats(){
        std::lock_guard<simple_mutex> __(*fgl_lock);
        for(auto itr = fgl_states->begin(); itr != fgl_states->end(); ++itr){
                _fgl_update_stats(fgl_stats, *itr);
        }
}

HEADER_INLINE void fgl_get_stats(struct fine_grained_locking_stats* stats){
        fgl_update_stats();
        *stats = *fgl_stats;
}

HEADER_INLINE struct fine_grained_locking_state* fgl_get_state(){
        struct fine_grained_locking_state* toRet = fgl_state;
        if (!toRet){
                std::lock_guard<simple_mutex> __(*fgl_lock);
                simple_allocator_allocate(toRet, fgl_allocator);
                new (toRet) struct fine_grained_locking_state({});
                fgl_state = toRet;
                fgl_states->push_back(fgl_state);
        }
        return toRet;
}

#define FGL_LOCK_GUARD(guard_name) \
        fine_grained_lock_guard __(record.lock); \
        fgl_get_state()->stats.nLocks++; \

//Currently no handling for an out of memory adding a lock to this list
//Whatever.
HEADER_INLINE void FGL_ADD_LOCK(fine_grained_lock_t* lock){
        struct fine_grained_locking_state* state = fgl_get_state();
        auto& lock_vec = state->lock_vec;
        lock_vec.emplace_back(lock);
}

HEADER_INLINE void FGL_LOCK_ALL(){
        struct fine_grained_locking_state* state = fgl_get_state();

        auto& lock_vec = state->lock_vec;

        std::sort(lock_vec.begin(), lock_vec.end());
        lock_vec.erase(
                std::unique(lock_vec.begin(), lock_vec.end()),
                lock_vec.end()
        );


        for(auto itr = lock_vec.begin(); itr != lock_vec.end(); ++itr){
                (*itr)->unlock();
        }


        fgl_get_state()->stats.nLocks+=lock_vec.size();
}

HEADER_INLINE void FGL_UNLOCK_ALL(){
        struct fine_grained_locking_state* state = fgl_get_state();

        auto& lock_vec = state->lock_vec;

        for(auto itr = lock_vec.begin(); itr != lock_vec.end(); ++itr){
                (*itr)->unlock();
        }
        lock_vec.clear();
}
