#include <linux/kthread.h>
#include <linux/delay.h>

#include "sivfs.h"
#include "sivfs_make_checkpoint.h"
#include "sivfs_gc_checkpoints.h"
#include "sivfs_shared_state.h"
#include "sivfs_stack.h"
#include "sivfs_state.h"
#include "sivfs_mm.h"

//Don't actually apply the change to the checkpoint page, just 
//make sure the proper leaf page is there and ready to be written to
//(specifically we ensure the leaf page is not a zero page)
//
//Basically used for adding pages to checkpoints after the fact as part of
//direct access
#define SIVFS_APPLY_CHANGE_CP_NOWRITE (1<<2)

static int sivfs_on_obsolete_page(
        struct sivfs_stack* obsoleted_pages,
        struct page* old_page 
){
        if (obsoleted_pages){
                return sivfs_stack_push(obsoleted_pages, old_page);
        } else {
                //Ok, have to add the page one-by-one to the gc (expensive, but
                //makes cleanup semantics easier)
                struct sivfs_stack fake_stack = {
                        .size = 1,
                        .capacity = 1,
                        .values = (void**)&old_page
                };
                return sivfs_gc_mark_pages_obsoleted(
                        &fake_stack,
                        NULL
                );
        }
}

//state parameter is just for stats
static int sivfs_apply_change_checkpoint(
        struct page** root_page_ptr,
        struct sivfs_state* state,
        struct sivfs_writeset_entry* ent,
        sivfs_ts checkpoint_ts,
        struct sivfs_stack* obsoleted_pages,
        int options
){
        int rc = 0;

        if (SIVFS_DEBUG_MAKE_CHECKPOINTS){
                dout(
                        "Applying %lu %lu := %llu to checkpoint %016llx",
                        ent->ino,
                        ent->file_offset,
                        ent->value,
                        page_to_pa(*root_page_ptr)
                );
        }

        struct sivfs_checkpoint_iter iter;
        rc = sivfs_checkpoint_iter_init(&iter, ent->ino, ent->file_offset);
        if (rc)
                goto error0;

        phys_addr_t root_pa = 0;
        if (*root_page_ptr){
                root_pa = page_to_pa(*root_page_ptr);
        }
        //This is a pointer to an opaque 64 bit quantity which is
        // - phys_addr_t* above the FOFFSET levels
        // - pud_t, pmd_t, pte_t otherwise (i.e. with flag bits set as well)
        PxD_ent* root_ptr = &root_pa;

        //Start from the top level down to 0, i.e. leaf level
        size_t level;
        for(level = SIVFS_CHECKPOINT_TOTAL_LEVELS - 1; ; ){
                //COW the current page, if the old page is of an older checkpoint

                //Page exists, old => New alloc, copy from old
                //Page exists, OK => No new alloc, no copy
                //No page exists => New alloc, fill with 0

                bool has_checkpoint_page = false;
                struct page* new_page = NULL;
                void* new_mem = NULL;
                if (likely(*root_ptr)){
                        struct page* old_page = pa_to_page(sivfs_pxd_to_pa(*root_ptr));
                        //Special handling for the sivfs_zero_pages
                        bool old_is_zero_page = sivfs_is_zero_page(old_page);
                        struct sivfs_checkpoint_pginfo * old_pginfo;
                        sivfs_ts old_pg_checkpoint_ts;
                        if (unlikely(old_is_zero_page)){
                                old_pg_checkpoint_ts = 0;
                        } else {
                                old_pginfo =
                                        sivfs_checkpoint_page_to_pginfo(old_page)
                                ;
                                old_pg_checkpoint_ts = old_pginfo->checkpoint_ts;
                        }
                        if (old_pg_checkpoint_ts == checkpoint_ts){
                                new_page = old_page;
                                new_mem = page_address(old_page);
                        } else {
                                rc = sivfs_new_checkpoint_page(
                                        &new_page,
                                        checkpoint_ts,
                                        level
                                );
                                if (rc)
                                        goto error0;

                                state->stats.nCheckpointPagesNew++;

                                new_mem = page_address(new_page);
                                void* old_mem = page_address(old_page);
                                memcpy(new_mem, old_mem, PAGE_SIZE);

                                if (!old_is_zero_page){
                                        //Mark the obsoleting timestamp of the old page
                                        //Note that the page is not necessarily
                                        //obsolete until we actually publish some
                                        //checkpoint with checkpoint_ts >= this obsoleting
                                        //timestamp. If we abort this checkpoint, we must
                                        //undo this assignment. (i.e. set all pages to
                                        //INVALID_TS obsoleted_ts!)
                                        old_pginfo->obsoleted_ts = checkpoint_ts;

                                        rc = sivfs_on_obsolete_page(obsoleted_pages, old_page);
                                        if (rc) {
                                                //Need to free the new
                                                //checkpoint page
                                                sivfs_put_checkpoint_page(new_page);
                                                goto error0;
                                        }
                                }
                        }
                } else {
                        rc = sivfs_new_checkpoint_page(&new_page, checkpoint_ts, level);
                        if (rc)
                                goto error0;

                        state->stats.nCheckpointPagesNew++;

                        new_mem = page_address(new_page);
                        sivfs_mk_zero_checkpoint_page(new_mem, level);
                }

                if (SIVFS_DEBUG_MAKE_CHECKPOINTS){
                        dout("Page %016llx is @ %zd", page_to_pa(new_page), level);
                }

                if (level == 0){
                        //Need to apply the change to the leaf page

                        if (unlikely(options & SIVFS_APPLY_CHANGE_CP_NOWRITE)){
                                //Application suppressed by option
                        } else {
                                sivfs_apply_ws_ent_pg(new_mem, ent);
                        }

                        //New page is never ZERO_PAGE so we can set index
                        pgoff_t pgoff = ent->file_offset >> PAGE_SHIFT;
                        new_page->index = pgoff;
                }

                //Reveal the new page
                phys_addr_t new_pa = __pa(new_mem);

                size_t level_parent = level + 1;
                if (level_parent == 1){
                        //Take away write privileges for leaf nodes (?)
                        *root_ptr = sivfs_mk_pte(new_pa);
                } else if (level_parent < SIVFS_CHECKPOINT_FOFFSET_LEVELS){
                        //write privileges probably required for higher directories
                        //just because of hardware details
                        *root_ptr = sivfs_mk_pxd_ent(new_pa);
                } else {
                        //Just a raw physical address (compatible with pxd)
                        *root_ptr = new_pa;
                }


                //Top level => output root_out
                if (level == SIVFS_CHECKPOINT_TOTAL_LEVELS - 1){
                        *root_page_ptr = new_page;
                }

                //Advance?
                if (level == 0){
                        //Done.
                        break;
                }

                //Continue.
                size_t offset = sivfs_checkpoint_iter_next_offset(&iter);
                PxD_ent* table = (PxD_ent*)new_mem;
                root_ptr = table + offset;
                sivfs_checkpoint_iter_advance(&iter);
                level--;
        }


error0:
        if (rc == -ENOMEM){
                dout("Error creating checkpoint - too many checkpoint pages!");
        }

        return rc;
}

int sivfs_prepare_direct_access_page(
        struct sivfs_state* state,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset
){
        int rc = 0;

        struct sivfs_inode_info *cpinfo = sivfs_inode_to_iinfo(checkpoint);

        struct sivfs_writeset_entry ent = {
               .ino = file_ino,
               .file_offset = file_offset
        };

        //Calling this will insert a non-zero checkpoint page at the
        //file_ino:foffset specified.
        rc = sivfs_apply_change_checkpoint(
                &cpinfo->cp_root,
                state,
                &ent,
                cpinfo->cp_ts,
                NULL,
                SIVFS_APPLY_CHANGE_CP_NOWRITE
        );

        if (rc){
                dout("Here");
                goto error0;
        }

error0:
        return rc;
}

#if 0
//COW a root node. Used to handle the corner case where between checkpoint
//intervals no writes occur (only occurs if all transactions aborted.)
//
//We want to copy the new root so that we can ensure that each root of each
//checkpoint is unique (makes memory collection / auditing easier)
static int _sivfs_force_new_root(
        struct page** root_page_ptr,
        sivfs_ts checkpoint_ts,
        struct sivfs_stack* obsoleted_pages
){
        int rc = 0;

        if (SIVFS_DEBUG_ABORTS){
                dout("Forcing new root due to aborted txns");
        }

        struct page* new_page = NULL;
        void* new_mem = NULL;

        rc = sivfs_new_checkpoint_page(
                &new_page,
                checkpoint_ts
        );
        if (rc)
                goto error0;
        new_mem = page_address(new_page);

        if (likely(*root_page_ptr)){
                struct page* old_page = *root_page_ptr;
                struct sivfs_checkpoint_pginfo* old_pginfo =
                        sivfs_checkpoint_page_to_pginfo(old_page)
                ;
                void* old_mem = page_address(old_page);
                memcpy(new_mem, old_mem, PAGE_SIZE);
                //Mark the obsoleting timestamp of the old page
                //Note that the page is not necessarily
                //obsolete until we actually publish some
                //checkpoint with checkpoint_ts >= this obsoleting
                //timestamp. If we abort this checkpoint, we must
                //undo this assignment. (i.e. set all pages to
                //INVALID_TS obsoleted_ts!)
                old_pginfo->obsoleted_ts = checkpoint_ts;
                rc = sivfs_stack_push(
                        obsoleted_pages,
                        old_page
                );
                if (rc)
                        goto error0;
        } else {
                memset(new_mem, 0, PAGE_SIZE);
        }

        //Output
        *root_page_ptr = new_page;

error0:
        return rc;
}
#endif

//Called one or more times to make a checkpoint
//TODO add a parameter that is used to parallelize
//root_ptr points to either the old root or a new root. Will CAS a replacement
//page if needed
//
//If an error occurs, should only clean up resources that are not pointed to by
//the new root. Those that are pointed to by the new root will be cleaned up by
//the caller, as the checkpoint will be undone.
static int _sivfs_make_checkpoint(
        struct page** root_ptr,
        struct sivfs_state* state,
        struct sivfs_stack* obsoleted_pages,
        struct sivfs_shared_state* sstate,
        sivfs_ts latest_checkpoint,
        sivfs_ts next_ts
){
        int rc = 0;

        if (next_ts == latest_checkpoint){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        //Log range from latest_checkpoint + 1 to next_ts
        //This is because the checkpoint already contains all changes of
        //latest_checkpoint
        //This also allows us to avoid the corner case of ts == 0, which does
        //not have a log entry
        struct sivfs_log_entry* log;
        rc = sivfs_ts_to_log_entry(
                &log,
                sstate,
                latest_checkpoint + 1,
                latest_checkpoint + 1,
                next_ts
        );
        if (rc)
                goto error0;
        if (log == NULL){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        while(log->ts <= next_ts){
                struct sivfs_writeset* ws = &log->ws;

                if (log->aborted){
                        //Skip aborted txns
                        if (SIVFS_DEBUG_MAKE_CHECKPOINTS){
                                dout("Skipping aborted txn in make_checkpoint");
                        }
                        goto continue_loop;
                }

                struct sivfs_writeset_entry* ent = ws->entries;
                size_t size_left = ws->__size_bytes;
                while(size_left){
                        struct sivfs_wse_stream_info info;
                        rc = sivfs_wse_to_stream_info(
                                &info,
                                ent,
                                size_left
                        );
                        if (rc){
                                dout("Here");
                                goto error0;
                        }

                        rc = sivfs_apply_change_checkpoint(
                                        root_ptr,
                                        state,
                                        ent,
                                        next_ts,
                                        obsoleted_pages,
                                        0
                        );
                        if (rc){
                                goto error0;
                        }

                        //Advance
                        size_t wse_size = sivfs_wse_size(&info);
                        ent = (struct sivfs_writeset_entry*)(
                                (char*)ent + wse_size
                        );
                        size_left -= wse_size;
                }

        continue_loop:
                //Advance log
                if (log->ts == next_ts){
                        break;
                }

                struct sivfs_log_entry* next_log = log->next;
                if (!next_log || next_log->ts > next_ts){
                        //With timestamps assigned to every integer, this
                        //never occurs, but may as well handle the more
                        //general case where we can non-sequential timestamps
                        break;
                }
                log = next_log;
        }

error0:
        return rc;
}

//Not MT safe, the caller will start creating a checkpoint
//if it looks like it can.
int sivfs_make_checkpoint(
        bool* made_checkpoint,
        struct sivfs_shared_state* sstate
){
        int rc = 0;

        struct sivfs_checkpoints* checkpoints = &sstate->checkpoints;

        //Only needed for stats, but fail anyway if we can't get it
        struct sivfs_state* state = sivfs_task_to_state(current);
        if (!state){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        //No need for double-read-protect, since this method is not MT safe
        sivfs_ts latest_checkpoint_ts = atomic64_read(&checkpoints->latest_checkpoint.ts);
        struct inode* latest_checkpoint = checkpoints->latest_checkpoint.checkpoint;

        sivfs_ts latest_commit = atomic64_read(&sstate->master_ts);
        //Term on the right of the indirection will not overflow
        //(latest_checkpoint starts at 0 at initialiation)
        //if (latest_commit <= latest_checkpoint + SIVFS_MIN_COMMIT_PER_SS){
        if (latest_commit == latest_checkpoint_ts){
                //Nothing to do
                *made_checkpoint = false;
                return rc;
        }

        //Past this point we will try to make a new checkpoint
        if (SIVFS_DEBUG_MAKE_CHECKPOINTS) {
                dout(
                        "Trying to make a checkpoint to version %llu",
                        latest_commit
                );
        }

        sivfs_ts next_ts = latest_commit;
        struct inode* prior_checkpoint = latest_checkpoint;
        //We insert an empty checkpoint at time 0 so the following sanity
        //check is sound:
        if (!prior_checkpoint){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_inode_info *prior_cpinfo =
                sivfs_inode_to_iinfo(prior_checkpoint)
        ;

        if (!prior_cpinfo){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        if (prior_cpinfo->cp_ts != latest_checkpoint_ts){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_stack* obsoleted_pages;
        rc = sivfs_new_stack(&obsoleted_pages);
        if (rc) {
                dout("Here");
                goto error0;
        }

        struct inode* new_checkpoint;
        rc = sivfs_create_checkpoint_inode(&new_checkpoint, sstate->sb);
        if (rc){
                dout("Here");
                goto error1;
        }

        struct sivfs_inode_info *cpinfo =
                sivfs_inode_to_iinfo(new_checkpoint)
        ;
        cpinfo->cp_root = prior_cpinfo->cp_root;
        cpinfo->cp_ts = next_ts;

        //TODO parallelize this
        //and wait for the subtasks to finish here (preferably by
        //yielding the CPU core since the subtasks can be long running)
        rc = _sivfs_make_checkpoint(
                &cpinfo->cp_root,
                state,
                obsoleted_pages,
                sstate,
                prior_cpinfo->cp_ts,
                next_ts
        );
        if (rc) {
                dout("Here");
                goto error2;
        }

#if 0
        //Ensure here that we actually made some changes, otherwise we need to
        //allocate a new root.
        if (old_root == new_root){
                //Silent write new_root to force it to be different
                rc = _sivfs_force_new_root(
                        &new_root,
                        next_ts,
                        obsoleted_pages
                );
                if (rc) {
                        dout("Here");
                        goto error1;
                }
        }
#endif

        rc = _sivfs_checkpoints_insert(
                checkpoints,
                new_checkpoint
        );
        if (rc){
                dout("Here");
                goto error2;
        }

        prior_cpinfo->obsoleted_ts = next_ts;
        rc = sivfs_gc_mark_pages_obsoleted(
                obsoleted_pages,
                prior_checkpoint
        );
        if (rc)
                goto error3;

        //Point of no return for this checkpoint, because we can't unmark the
        //old pages as obsoleted easily

out:
        //On success, publish the result:
        _sivfs_checkpoints_insert_finish(
                checkpoints,
                new_checkpoint
        );

        *made_checkpoint = true;
        if (SIVFS_DEBUG_MAKE_CHECKPOINTS){
                dout("Successfully made checkpoint to %llu", next_ts);
        }

        //Stats
        state->stats.nCheckpoints++;
        state->stats.nCheckpointsTxns+=(next_ts - latest_checkpoint_ts);

        //We have created a new checkpoint
        //=> wake up gc
        wake_up_interruptible(&sstate->gc_wq);

        //Free obsoleted_pages stack
        sivfs_put_stack(obsoleted_pages);
        return rc;

error3:
        //Must remove the checkpoint entry, otherwise the next checkpoint that
        //updates latest_ts past it will also publish the aborted checkpoint
        _sivfs_checkpoints_remove(checkpoints, new_checkpoint);
        dout("In error2");

error2:
        //Must go through new_root recursively and free any pages with
        //checkpoint_ts == next_ts

        //Might want to go through obsoleted pages and set their obsoleted_ts to
        //infinity
        //Though It's not necessary for correctness I think

        //Finally remove the checkpoint
        iput(new_checkpoint);

error1:
        sivfs_put_stack(obsoleted_pages);

        dout("Cleanup after OOM in making checkpoint not yet implemented - MEMORY LEAK");

error0:
        //If we are out of memory suppress the error and sleep a bit, hoping
        //that memory is reclaimed next time we are woken up to do work.
        //
        //Note that we don't set make_checkpoint, which means we won't
        //repeatedly retry in an out of memory state unless new commits
        //repeatedly wake us up - this is a safety measure to avoid infinitely
        //retrying checkpoint creation if we are terminally out of memory. We
        //would like to retry soon after the OOM is resolved, but there is no
        //way to listen for such a signal so retrying on the next commit is a
        //reasonable best-effort solution.
        if (rc == -ENOMEM){
                dout("Throttling checkpoint creation due to OOM.");
                cond_resched();
                msleep(1);
                rc = 0;
        }

        return rc;
}

int sivfs_make_checkpoint_threadfn(void* _sstate)
{
        int rc = 0;
        bool exit_after_cleanup = false;
        struct sivfs_shared_state* sstate = _sstate;

        if (!sstate){
                dout("Here");
                goto error_spin0;
        }

#if 0
        //for testing
        int i;
        for(i = 0; i < 1000000; i++){
                struct page* page = NULL;
                sivfs_new_checkpoint_page(&page, 0);
                //page = alloc_page(GFP_KERNEL);
                sivfs_put_checkpoint_page(page);
        }

        rc = -ENOMEM;
        goto error0;
#endif

        //TODO parallelize, assign next checkpoint worker ID via atomics
        unsigned long id = 0;

        dout("Starting make_checkpoints thread id %lu", id);

        //Initialize state
        rc = sivfs_get_task_state(current);
        if (rc){
                goto error_spin0;
        }

        DEFINE_WAIT(wait);
        while(!kthread_should_stop()){
                bool made_checkpoint = false;
                rc = sivfs_make_checkpoint(&made_checkpoint, sstate);
                if (rc) {
                        goto error_spin1;
                }

                //We want to come back after some time, allowing the core to do
                //useful work. We want to break out early on signals in case
                //writers try to wake us up.
                ktime_t kmin = ktime_set(
                        0,
                        SIVFS_CHECKPOINT_UDELAY_MIN*NSEC_PER_USEC
                );
                schedule_hrtimeout_range(
                        &kmin,
                        SIVFS_CHECKPOINT_UDELAY_DELTA,
                        HRTIMER_MODE_REL
                );
                //cond_resched();
                //udelay(SIVFS_CHECKPOINT_UDELAY_MIN);

                if (!made_checkpoint){
                        //Do a longer sleep using make_checkpoint_wq
                        prepare_to_wait(
                                &sstate->make_checkpoint_wq,
                                &wait,
                                TASK_INTERRUPTIBLE
                        );

                        //Whether or not we succeed here doesn't matter
                        //we are just finishing off any work staged before we
                        //called prepare_to_wait
                        rc = sivfs_make_checkpoint(&made_checkpoint, sstate);
                        if (rc) {
                                finish_wait(&sstate->make_checkpoint_wq, &wait);
                                goto error_spin1;
                        }
                        if (kthread_should_stop()){
                                finish_wait(&sstate->make_checkpoint_wq, &wait);
                                goto out;
                        }
                        //This will NOT sleep if we received any signals since the
                        //last prepare_to_wait, and we checked kthread_should_stop
                        //since then.
                        //
                        //Skipping the sleep if we end up making a checkpoint
                        //ensures we bounce back faster after a long pause
                        if (!made_checkpoint){
                                schedule();
                        }
                        finish_wait(&sstate->make_checkpoint_wq, &wait);
                }
        }

out:
        exit_after_cleanup = true;

error_spin1:
        //Clean up task state
        sivfs_put_task_state(current);

error_spin0:
        if (exit_after_cleanup){
                return rc;
        }

        //Otherwise we jumped to one of the error labels and need
        //to spin so that the parent can safely kill our task struct
        dout("ERROR: stopping checkpointing");
        set_current_state(TASK_INTERRUPTIBLE);
        while(!kthread_should_stop()){
                schedule();
                set_current_state(TASK_INTERRUPTIBLE);
        }
        return rc;
}
