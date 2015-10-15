#include <linux/kthread.h>

#include "sivfs_background_threads.h"
#include "sivfs_shared_state.h"

#include "sivfs_make_checkpoint.h"
#include "sivfs_gc_checkpoints.h"
#include "sivfs_gc_log.h"

static int sivfs_bgt_run(
        struct task_struct** bgt_out,
        int (*threadfn)(void*),
        void* arg,
        const char* threadname
){
        int rc = 0;

        struct task_struct* bgt = kthread_run(
                threadfn,
                arg,
                "%s",
                threadname
        );
        if (IS_ERR(bgt)){
                rc = PTR_ERR(bgt);
                goto error0;
        }

out:
        *bgt_out = bgt;
        return rc;

error0:
        return rc;
}

static void sivfs_bgt_stop(struct task_struct* bgt){
        if (!bgt){
                dout("Assertion error.");
                return;
        }

        //kthread_stop segfaults if bgt is already dead. Avoid that with a more
        //informative WARN:
        if (bgt->exit_state){
                WARN(
                        true,
                        "BG thread died unexpectedly (%.*s). "
                        "It might not be fully cleaned up.",
                        TASK_COMM_LEN, bgt->comm
                );
                return;
        }

        //dout("Stopping a bg thread with state %lx %x", bgt->state, bgt->exit_state);
        kthread_stop(bgt);
}

int sivfs_init_background_work(struct sivfs_shared_state* sstate)
{
        int rc = 0;

        //Start snapshotting
        /*
        queue_delayed_work(
                sstate->snapshot_wq,
                &sstate->make_checkpoint_work,
                SIVFS_SS_DELAY
        );
        */
        rc = sivfs_bgt_run(
                &sstate->make_checkpoint_bgt,
                sivfs_make_checkpoint_threadfn,
                sstate,
                "sivfs_make_checkpoints"
        );
        if (rc)
                goto error0;
        //Start GC'ing
        rc = sivfs_bgt_run(
                &sstate->gc_checkpoints_bgt,
                sivfs_gc_checkpoints_threadfn,
                sstate,
                "sivfs_gc_checkpoints"
        );
        if (rc)
                goto error1;

        rc = sivfs_bgt_run(
                &sstate->gc_log_bgt,
                sivfs_gc_log_threadfn,
                sstate,
                "sivfs_gc_log"
        );
        if (rc)
                goto error2;

out:
        return rc;

error2:
        sivfs_bgt_stop(sstate->gc_checkpoints_bgt);

error1:
        sivfs_bgt_stop(sstate->make_checkpoint_bgt);

error0:
        return rc;
}

void sivfs_destroy_background_work(struct sivfs_shared_state* sstate)
{
        //Stop the delayed works.
        //Acc. to spec, this is safe even if the snapshot work rearms itself
        //with queue_delayed_work. In that case, when this returns it is
        //guaranteed that the function is not running and will not run
        //(assuming some other thread does not concurrently rearm it for some
        //reason, but this should not happen)
        //cancel_delayed_work_sync(&sstate->make_checkpoint_work);
        //Stop the GC work
        //cancel_delayed_work_sync(&sstate->gc_checkpoints_work);
        //Stop bgts
        dout("Stopping bgts...");
        sivfs_bgt_stop(sstate->make_checkpoint_bgt);
        sivfs_bgt_stop(sstate->gc_checkpoints_bgt);
        sivfs_bgt_stop(sstate->gc_log_bgt);
        dout("Stopped bgts.");
}
