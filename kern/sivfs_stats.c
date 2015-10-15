#include "sivfs_stats.h"
#include "sivfs_state.h"
#include "sivfs_shared_state.h"
#include "sivfs_radix_tree.h"

//Helper method for sivfs_update_stats
void _sivfs_update_stats(struct sivfs_stats* stats, struct sivfs_state* state){

        //Copy stats onto the stack. This is the snapshot.
        struct sivfs_stats stats_snapshot = state->stats;

#define F(x) { \
        stats->x += (stats_snapshot.x - state->stats_last_snapshot.x); \
}

        SIVFS_STATS
#undef F

        //Finally, update the snapshot:
        state->stats_last_snapshot = stats_snapshot;
}

//Iterates over all running threads and accumulates changes in stats
//to the global stats
void sivfs_update_stats(void){
        struct sivfs_shared_state* sstate;
        sstate = &sivfs_shared_state;

        //Need threads_lock here because otherwise we might observe a
        //sivfs_stats that is being destroyed (plus not 100% that radix tree
        //iteration can be done with just an RCU lock)
        spin_lock(&sstate->threads_lock);

        //Stats lock serializes all calls to this method, so the trick of
        //stats + stats_last_snapshot used in sivfs_state works since we
        //atomically update stats_last_snapshot := stats on each update
        spin_lock(&sstate->stats_lock);

        void** slot;
        struct radix_tree_iter iter;
        radix_tree_for_each_slot(
                slot,
                &sstate->threads,
                &iter,
                0
        )
        {
                struct sivfs_state* state = *slot;
                _sivfs_update_stats(&sstate->stats, state);
        }


        spin_unlock(&sstate->stats_lock);
        spin_unlock(&sstate->threads_lock);

        if (SIVFS_DEBUG_STATS) {
#define F(x) dout(#x "= %llu", sstate->stats.x);
                SIVFS_STATS
#undef F
        }
}
