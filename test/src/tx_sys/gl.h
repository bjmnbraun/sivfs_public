#pragma once

#include <vector>
#include <mutex>

#include "txsys_config.h"

#include "common.h"
#include "simple_allocator.h"
#include "simple_mutex.h"

//Global lock implementation of STM
//This is terribly inefficient since all but one thread is always stalling on a
//lock. Never use!

struct gl_stats {
        unsigned long nCommits;
};

struct gl_state {
        bool inTxn;
        struct gl_stats stats;
        struct gl_stats stats_last_snapshot;
};

#define GL_STATS \
        F(nCommits)

//mutex gives good performance under high contention
#if GL_MUTEX_TYPE == GL_SPIN
typedef simple_mutex gl_lock_t;
#elif GL_MUTEX_TYPE == GL_MUTEX
typedef simple_spinlock gl_lock_t;
#else
        Assertion error
#endif
extern gl_lock_t* gl_lock;

//Process-based implementation, not thread-based.
extern thread_local struct gl_state* gl_state;
extern struct gl_stats* gl_stats;

//gl_lock must be held to access any of the below
extern vector_simple_allocator<struct gl_state*>* gl_states;
extern _simple_allocator* gl_allocator;

HEADER_INLINE void gl_init(/*shared*/ void* shared_space, size_t size_shared_space){
        _simple_allocator* allocator;
        if (size_shared_space < sizeof(*allocator)){
                assert(0);
        }
        allocator = (decltype(allocator))shared_space;

        shared_space = (char*)shared_space + sizeof(*allocator);
        size_shared_space -= sizeof(*allocator);
        new (allocator) _simple_allocator(shared_space, size_shared_space);
        gl_allocator = allocator;

        simple_allocator_allocate(gl_lock, allocator);
        new (gl_lock) gl_lock_t();

        simple_allocator_allocate(gl_states, allocator);
        new (gl_states) vector_simple_allocator<struct gl_state*>(allocator);

        simple_allocator_allocate(gl_stats, allocator);
        new (gl_stats) struct gl_stats({});
}

HEADER_INLINE void _gl_update_stats(struct gl_stats* stats, struct gl_state* state){
        struct gl_stats stats_snapshot = state->stats;

#define F(X) \
        stats->X += stats_snapshot.X - state->stats_last_snapshot.X; 

        GL_STATS 

#undef F
        //Reset snapshot
        state->stats_last_snapshot = stats_snapshot;
}

HEADER_INLINE void gl_update_stats(){
        std::lock_guard<std::remove_pointer_t<decltype(gl_lock)>> __(*gl_lock);
        for(auto itr = gl_states->begin(); itr != gl_states->end(); ++itr){
                _gl_update_stats(gl_stats, *itr);
        }
}

HEADER_INLINE void gl_get_stats(struct gl_stats* stats){
        gl_update_stats();
        *stats = *gl_stats;
}

HEADER_INLINE struct gl_state* gl_get_state(){
        struct gl_state* toRet = gl_state;
        if (!toRet){
                //Then we are not in a txn so safe to grab gl_lock
                std::lock_guard<std::remove_pointer_t<decltype(gl_lock)>> __(*gl_lock);
                simple_allocator_allocate(toRet, gl_allocator);
                new (toRet) struct gl_state({});
                gl_state = toRet;
                gl_states->push_back(gl_state);
        }
        return toRet;
}

HEADER_INLINE void gl_commit_update(/*out*/ bool& aborted){
        struct gl_state* state = gl_get_state();
        if (state->inTxn){
                state->stats.nCommits++;
                gl_lock->unlock();
        } else {
                state->inTxn = true;
        }
        //With mutex, this should fairly round robin ownership of the
        //lock
        gl_lock->lock();

        aborted = false;
}

