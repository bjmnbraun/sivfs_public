#pragma once

#include <vector>
#include <mutex>

#include "txsys_config.h"

#include "common.h"
#include "simple_allocator.h"
#include "simple_mutex.h"

//Locktable actually encompasses three related algorithms
// - Locktable
// - TinySTM
// - Silo (adapted)
//
// In all three cases, we have a table of locks (ownership records)
// In TinySTM and Silo the table entries have, in addition to a lock,
// a timestamp used for read validation.
//
// The function that maps addresses to table entries and the size of the table
// as well as "hierarchical" tricks apply to all three algorithms
//
// TinySTM guarantees consistent reads mid-transaction
// Locktable ^ + guarantees transaction will commit if reach commit
// Silo doesn't guarantee consistent reads mid-transaction
//
// Theory question: Can we prove that consistent reads are _necessary_ to
// implement a LL?
//
//  - Almost definitely false. Give the txn a timeout, and after that timeout
//  validate. If fail, keep timeout the same, if success, then the txn just
//  needs longer so double the timeout. Either way repeat until success.
//   Eventually the timeout will exceed the time it takes to do the transaction
//   assuming it commits, and then in that state it will only retry if there
//   is a real conflict and in bounded time. So the transaction will be
//   performant.
//
// Empirical variant: Give a repeatable interleaving that causes bad behavior
// even though "reasonable measures" are taken to deal with inconsistent reads
//
//  - Also not really possible. As long as you prevent nullpointerexceptions
//  and treat all exceptions as aborts you can run and revalidate whenever
//  the readset hits a power of two in size. The total validation cost
//  is linear in the number of reads, then, and the cost of a cycle
//  is at most double the duration of the transaction otherwise.

class locktable_aborted_exception : virtual public std::exception{
};

struct locktable_stats {
        unsigned long nTxns;
        unsigned long nCommits;
        unsigned long nValidations;
        unsigned long nValidatedReadEntries;
        unsigned long nMidTxnValidations;
};

#define LOCKTABLE_STATS \
        F(nTxns) \
        F(nCommits) \
        F(nValidations) \
        F(nValidatedReadEntries) \
        F(nMidTxnValidations) \

typedef uint64_t locktable_ts;
struct locktable_read_entry {
       uint64_t lockid;
       locktable_ts ts;
};
struct locktable_write_entry {
       uint64_t address;
       uint64_t undo_value;
};

struct locktable_state {
        bool inTxn;
        uint64_t thr_id;
        std::vector<uint64_t> locks_held;
        std::vector<struct locktable_read_entry> readset;
        std::vector<struct locktable_write_entry> writeset;
        struct locktable_stats stats;
        struct locktable_stats stats_last_snapshot;
        bool aborted;
        locktable_ts commit_ts;
        locktable_ts readset_validated_ts;
};

struct locktable_table_entry {
        std::atomic<uint64_t> lock_ownership;
        //Last written TS
        std::atomic<uint64_t> ts;
};

class locktable_shared_state {
public:
        locktable_shared_state(_simple_allocator* allocator)
        : shared_state_lock()
        , stats{}
        , states{allocator}
        , last_thr_id{1}
        {
        }

        //0 is not a valid thr_id, so 0 means unlocked
        struct locktable_table_entry locktable [LOCKTABLE_SIZE];

        //Used for TinySTM, a global clock.
        //Silo uses this only for logging purposes, without logging
        //silo does not use this
        std::atomic<locktable_ts> global_ts;

        //Lock for the following fields
        simple_mutex shared_state_lock;
        struct locktable_stats stats;
        vector_simple_allocator<struct locktable_state*> states;
        uint64_t last_thr_id;
};

//Process-based implementation, not thread-based.
extern thread_local struct locktable_state* locktable_state;

extern locktable_shared_state* locktable_sstate;
extern _simple_allocator* locktable_allocator;

HEADER_INLINE void locktable_init(
        /*shared*/ void* shared_space,
        size_t size_shared_space
){
        _simple_allocator* allocator;
        if (size_shared_space < sizeof(*allocator)){
                assert(0);
        }
        allocator = (decltype(allocator))shared_space;

        shared_space = (char*)shared_space + sizeof(*allocator);
        size_shared_space -= sizeof(*allocator);
        new (allocator) _simple_allocator(shared_space, size_shared_space);
        locktable_allocator = allocator;

        simple_allocator_allocate(locktable_sstate, allocator);
        new (locktable_sstate) locktable_shared_state(allocator);
}

HEADER_INLINE void _locktable_update_stats(
        struct locktable_stats* stats,
        struct locktable_state* state
){
        struct locktable_stats stats_snapshot = state->stats;

#define F(X) \
        stats->X += stats_snapshot.X - state->stats_last_snapshot.X;

        LOCKTABLE_STATS

#undef F
        //Reset snapshot
        state->stats_last_snapshot = stats_snapshot;
}

HEADER_INLINE void locktable_update_stats(){
        auto& lock = locktable_sstate->shared_state_lock;
        auto& states = locktable_sstate->states;
        auto& stats = locktable_sstate->stats;

        std::lock_guard<decltype(lock)> __(lock);
        for(auto itr = states.begin(); itr != states.end(); ++itr){
                _locktable_update_stats(&stats, *itr);
        }
}

HEADER_INLINE void locktable_get_stats(struct locktable_stats* stats){
        locktable_update_stats();
        *stats = locktable_sstate->stats;
}

HEADER_INLINE struct locktable_state* locktable_get_state(){
        struct locktable_state* toRet = locktable_state;
        if (!toRet){
                auto& lock = locktable_sstate->shared_state_lock;
                auto& states = locktable_sstate->states;
                //Then we are not in a txn so safe to grab locktable_lock
                std::lock_guard<decltype(lock)> __(lock);
                simple_allocator_allocate(toRet, locktable_allocator);
                new (toRet) struct locktable_state({});
                toRet->thr_id = locktable_sstate->last_thr_id++;
                locktable_state = toRet;
                states.push_back(locktable_state);
        }
        return toRet;
}

HEADER_INLINE uint64_t locktable_addr_to_lock(void* address){
        //Cache line false sharing.
        uint64_t t1 = (uint64_t)address;
        t1 /= 64;
        //Hash
        uint64_t t2 = (t1*33)%LOCKTABLE_SIZE;
        return t2;
}

HEADER_INLINE void _locktable_validate(
        struct locktable_state* state,
        const struct locktable_read_entry& rdent
){
        auto& sstate = *locktable_sstate;
        auto& entry = sstate.locktable[rdent.lockid];
        state->stats.nValidatedReadEntries++;
        uint64_t owner = entry.lock_ownership.load(
                std::memory_order_relaxed
        );
        locktable_ts ts = entry.ts.load(std::memory_order_relaxed);

        if (owner != 0 && owner != state->thr_id){
                goto failed;
        }
        if (rdent.ts != ts){
                goto failed;
        }

        //Success
        return;

failed:
        state->aborted = true;
}

HEADER_INLINE void locktable_validate(struct locktable_state* state){
#if LOCKTABLE_ALG == LOCKTABLE_SILO || LOCKTABLE_ALG == LOCKTABLE_TINYSTM
        auto& readset = state->readset;
        auto& sstate = *locktable_sstate;
        state->stats.nValidations++;
        state->stats.nMidTxnValidations++;
        for(auto itr = readset.begin(); itr != readset.end(); ++itr){
                _locktable_validate(state, *itr);
        }
        if (state->aborted){
                throw locktable_aborted_exception();
        }

#elif LOCKTABLE_ALG == LOCKTABLE_LOCKTABLE
        //No-op for locktable alg
#else
Assertion error
#endif
}

HEADER_INLINE void locktable_add_read_set_entry(
        struct locktable_state* state,
        uint64_t lockid,
        locktable_ts ts
) {
#if LOCKTABLE_ALG == LOCKTABLE_TINYSTM
        if (ts > state->readset_validated_ts){
                //Need to revalidate old readset.
                locktable_ts validate_ts = locktable_sstate->global_ts.load(
                        std::memory_order_relaxed
                );

                locktable_validate(state);

                //OK.
                state->readset_validated_ts = validate_ts;
        }
#endif

        struct locktable_read_entry read_entry {
                .lockid = lockid,
                .ts = ts
        };
        state->readset.push_back(read_entry);
}

HEADER_INLINE void locktable_locktable_access(
        struct locktable_state* state,
        void* address
){
        uint64_t lockid = locktable_addr_to_lock((void*)address);

        auto& entry = locktable_sstate->locktable[lockid];
        auto& lock_ownership = entry.lock_ownership;

        //Most likely, we do not have the lock held.
        uint64_t expected = 0;
        bool could_lock = lock_ownership.compare_exchange_strong(
                expected,
                state->thr_id
        );

        if (could_lock){
                state->locks_held.push_back(lockid);

                //Need to add a read set entry for each written word
                //Necessary to achieve snapshot isolation or higher
                //
                //EX: T1 writes to A and B
                //    T2 reads from A writes B
                //    Without this we could commit T2 after T1 but
                //    where T2 does not read T1's write to A.
                locktable_add_read_set_entry(state, lockid, entry.ts);
        } else {
                //Possibly we already own it.
                if (expected == state->thr_id){
                        //OK!
                } else {
                        //Conflict.
                        state->aborted = true;
                        throw locktable_aborted_exception{};
                }
        }
}

HEADER_INLINE void locktable_write(volatile void* address, uint64_t value){
        struct locktable_state* state = locktable_get_state();
        locktable_locktable_access(state, (void*)address);

        // need to also add write set entry
        struct locktable_write_entry write_entry {
                .address = (uint64_t)address,
                .undo_value = ((uint64_t*)address)[0],
        };
        state->writeset.push_back(write_entry);

        //Finally do the actual write
        ((uint64_t*)address)[0] = value;
}
HEADER_INLINE void locktable_write(volatile void* address, void* value){
        locktable_write(address, (uint64_t)value);
}

template <class T>
HEADER_INLINE T locktable_read(volatile T* address){
        static_assert(sizeof(T) == 8, "Locktable only supports 8-byte word reads");

        struct locktable_state* state = locktable_get_state();
#if LOCKTABLE_ALG == LOCKTABLE_SILO || LOCKTABLE_ALG == LOCKTABLE_TINYSTM
        //Lockless read trick
        uint64_t lockid = locktable_addr_to_lock((void*)address);
        auto& entry = locktable_sstate->locktable[lockid];
        uint64_t owner = entry.lock_ownership.load(std::memory_order_relaxed);
        locktable_ts ts = entry.ts.load(std::memory_order_relaxed);
        T value = ((T*)address)[0];
        //Validate
        if (entry.ts.load(std::memory_order_relaxed) != ts){
                goto failed;
        }
        if (entry.lock_ownership.load(std::memory_order_relaxed) != owner){
                goto failed;
        }
        if (owner != 0 && owner != state->thr_id){
                //Can't read something that is locked by others
                goto failed;
        }

        //Success, try to add (may trigger revalidation which might abort)
        locktable_add_read_set_entry(state, lockid, ts);
        return value;

failed:
        state->aborted = true;
        throw locktable_aborted_exception{};

#elif LOCKTABLE_ALG == LOCKTABLE_LOCKTABLE
        locktable_locktable_access(state, (void*)address);
        return (T)(((uint64_t*)address)[0]);
#else
Assertion error
#endif
}


HEADER_INLINE void locktable_validate_pick_ts(struct locktable_state* state){
#if LOCKTABLE_ALG == LOCKTABLE_SILO || LOCKTABLE_ALG == LOCKTABLE_TINYSTM
        auto& readset = state->readset;
        auto& sstate = *locktable_sstate;
        state->stats.nValidations++;
        locktable_ts read_ts = 0;
        for(auto itr = readset.begin(); itr != readset.end(); ++itr){
#if LOCKTABLE_ALG == LOCKTABLE_SILO
                auto& entry = sstate.locktable[itr->lockid];
                locktable_ts ts = entry.ts.load(std::memory_order_relaxed);
                read_ts = std::max(read_ts, ts);
#endif
                _locktable_validate(state, *itr);
        }
        readset.clear();

#if LOCKTABLE_ALG == LOCKTABLE_SILO
        state->commit_ts = read_ts + 1;
#elif LOCKTABLE_ALG == LOCKTABLE_TINYSTM
        state->commit_ts = sstate.global_ts.fetch_add(1) + 1;
#endif

#elif LOCKTABLE_ALG == LOCKTABLE_LOCKTABLE
        //No-op for locktable alg
#else
Assertion error
#endif
}

//Used to abort a transaction, locktable uses write through so we need to
//apply undo entries
HEADER_INLINE void locktable_apply_undo_entries(
        struct locktable_state* state
){
        auto& writeset = state->writeset;
        //Need to apply in reverse order
        for(auto itr = writeset.rbegin(); itr != writeset.rend(); ++itr){
                void* address = (void*)itr->address;
                uint64_t undo_value = itr->undo_value;
                ((uint64_t*)address)[0] = undo_value;
        }
        writeset.clear();
}

HEADER_INLINE void locktable_unlock_table_entry(
        uint64_t lockid,
        struct locktable_state* state
){
        auto& entry = locktable_sstate->locktable[lockid];
        //For TinySTM and Silo, get commit TS from state and write it
#if LOCKTABLE_ALG == LOCKTABLE_SILO || LOCKTABLE_ALG == LOCKTABLE_TINYSTM
        entry.ts.store(state->commit_ts, std::memory_order_relaxed);
#endif

        //Finally unlock
        entry.lock_ownership.store(0, std::memory_order_relaxed);
}

HEADER_INLINE void locktable_commit_update(/*out*/ bool& aborted){
        struct locktable_state* state = locktable_get_state();
        if (/*likely*/ state->inTxn){
                //TinySTM and Silo validate here and pick a TS
                locktable_validate_pick_ts(state);
                state->readset.clear();

                if (state->aborted){
                        //Apply undo entries
                        locktable_apply_undo_entries(state);
                        aborted = true;
                        state->aborted = false;
                } else {
                        //Throw away writeset
                        state->writeset.clear();
                        state->stats.nCommits++;
                        aborted = false;
                }

                //unlock all locks
                //For TinySTM and Silo, we also set a TID here
                auto& locks_held = state->locks_held;
                for(
                        auto itr = locks_held.begin();
                        itr != locks_held.end();
                        ++itr)
                {
                        locktable_unlock_table_entry(*itr, state);
                }
                locks_held.clear();

                state->stats.nTxns++;
        } else {
                state->inTxn = true;
                aborted = false;
        }

#if LOCKTABLE_ALG == LOCKTABLE_TINYSTM
        //On starting a txn, we are validated up to global_ts.
        state->readset_validated_ts = locktable_sstate->global_ts.load(
                std::memory_order_relaxed
        );
#endif
}

HEADER_INLINE void locktable_abort(){
        struct locktable_state* state = locktable_get_state();
        state->aborted = false;
        throw locktable_aborted_exception{};
}
