#include <linux/kthread.h>
#include <linux/delay.h>

#include "sivfs_gc_log.h"
#include "sivfs_shared_state.h"
#include "sivfs_radix_tree.h"
#include "sivfs_state.h"
#include "sivfs_stack.h"

//Runs parallel to snapshot generation and cleans up old snapshot pages
int sivfs_gc_log(bool* did_gc_out){
        int rc = 0;
        bool did_gc = false;
        struct sivfs_shared_state* sstate = &sivfs_shared_state;
        struct sivfs_checkpoints* checkpoints = &sstate->checkpoints;

        //Step 1: Read from some recently published snapshot
        //Let this be MS for master snapshot
        sivfs_ts latest_snapshot = atomic64_read(&checkpoints->latest_checkpoint_published);

        //Step 2: Pull up all thread's reported checked out versions and
        //checked out snapshot versions, let these be the s_i
        struct sivfs_stack* tmp_checked_out_versions;
        rc = sivfs_new_stack(&tmp_checked_out_versions);
        if (rc)
                goto error0;

        rc = sivfs_get_checked_out_versions(tmp_checked_out_versions);
        if (rc)
                goto error_free;

        sivfs_ts stop_ts = latest_snapshot;

        size_t i = 0;
        for(i = 0; i < tmp_checked_out_versions->size; i++){
                sivfs_ts checked_out_ts =
                        (sivfs_ts)tmp_checked_out_versions->values[i]
                ;
                stop_ts = min(stop_ts, checked_out_ts);
        }

        sivfs_delete_log(&did_gc, sstate, stop_ts);

out:
        *did_gc_out = did_gc;

error_free:
        sivfs_put_stack(tmp_checked_out_versions);

error0:
        return rc;
}

int sivfs_gc_log_threadfn(void* ignored)
{
        int rc = 0;
        struct sivfs_shared_state* sstate = &sivfs_shared_state;

        dout("Starting gc_log thread");

        DEFINE_WAIT(wait);
        while(!kthread_should_stop()){
                bool did_gc = false;
                rc = sivfs_gc_log(&did_gc);
                if (rc) {
                        goto error_spin;
                }

                //Wait for an SLA period and then go again
                msleep_interruptible(SIVFS_LOG_GC_MDELAY);
                if (!did_gc){
                        //Do a longer sleep using gc_wq
                        prepare_to_wait(
                                &sstate->gc_wq,
                                &wait,
                                TASK_INTERRUPTIBLE
                        );
                        //Whether or not we succeed here doesn't matter
                        //we are just finishing off any work staged before we
                        //called prepare_to_wait
                        rc = sivfs_gc_log(&did_gc);
                        if (rc){
                                finish_wait(&sstate->gc_wq, &wait);
                                goto error_spin;
                        }
                        if (kthread_should_stop()){
                                finish_wait(&sstate->gc_wq, &wait);
                                goto out;
                        }
                        //This will NOT sleep if we received any signals since
                        //the last prepare_to_wait, and we checked
                        //kthread_should_stop since then.
                        schedule();
                        finish_wait(&sstate->gc_wq, &wait);
                }
        }

out:
        return rc;

error_spin:
        //Need to spin on error
        dout("ERROR: stopping log garbage collection");
        set_current_state(TASK_INTERRUPTIBLE);
        while(!kthread_should_stop()){
                schedule();
                set_current_state(TASK_INTERRUPTIBLE);
        }
        return rc;
}
