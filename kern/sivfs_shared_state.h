#ifndef SIVFS_SHARED_STATE_H
#define SIVFS_SHARED_STATE_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/radix-tree.h>

#include "sivfs_types.h"
#include "sivfs_log.h"
#include "sivfs_checkpoints.h"
#include "sivfs_stats.h"

/* Information associated with a superblock. */

struct cred;

//Member of sivfs_shared_state containing mount options
struct sivfs_mount_opts {
        umode_t mode;
};

struct sivfs_shared_state {
        //Remember to have an rcu_read lock when dereferencing this pointer or
        //the log_cache
        //master_log is "ahead" of master_ts, and contains the next
        //(potentially speculative) log being committed
        struct sivfs_log master_log;
        struct radix_tree_root log_cache;

        //Timestamp before which master_log has been GC'ed.
        //Used only for sanity checking that GC is not running ahead.
        sivfs_ts master_log_deleted_ts;

        //The latest committed version.
        atomic64_t master_ts;

        //Locks used for commit processing
        spinlock_t commit_lock; //held briefly at start of commit
        atomic64_t commit_started_ts; //hands out commit numbers

        //Snapshot of master log
        struct sivfs_checkpoints checkpoints;

        //Special creds used as an argument to vfs_open when attempting to
        //directly access the file, used by sivfs_commit_update to apply
        //changes to the underlying file
        //Is based on the credentials of the user who initializes the sivfs
        //module (and hence registers the filesystem) but this shouldn't
        //matter
        struct cred* direct_access_creds;

        //struct file*'s are destroyed before task_structs go away, so safe to
        //index this on value of "current"
        //radix tree of struct sivfs_state, indexed on value of task_struct
        //(current)
        struct radix_tree_root threads;
        //Lock for above tree
        spinlock_t threads_lock;

        //Background thread for making checkpoints
        struct task_struct* make_checkpoint_bgt;
        //Wait queue to wake up
        wait_queue_head_t make_checkpoint_wq;

        //Background thread for gc_checkpoints
        struct task_struct* gc_checkpoints_bgt;
        //Background thread for gc_log
        struct task_struct* gc_log_bgt;
        //Wait queue to wake up both gc threads
        wait_queue_head_t gc_wq;

        //Latest stats snapshot, updated periodically on demand and
        //whenever a sivfs_state is destroyed
        struct sivfs_stats stats;
        //Lock for above stats
        spinlock_t stats_lock;

        struct sivfs_mount_opts mount_opts;

        //The super block this shared state is associated with
        struct super_block *sb;
};

//One global copy, TODO we actually want one of these per mount
extern bool sivfs_shared_state_inited;
extern struct sivfs_shared_state sivfs_shared_state;

int sivfs_init_shared_state(
        struct sivfs_shared_state* sstate,
        struct super_block* sb,
        struct sivfs_mount_opts* mount_opts
);
void sivfs_destroy_shared_state(struct sivfs_shared_state* sstate);

HEADER_INLINE struct sivfs_shared_state* sivfs_sb_to_shared_state(
        struct super_block *sb
){
        return sb->s_fs_info;
}

HEADER_INLINE struct sivfs_shared_state* sivfs_inode_to_shared_state(
        struct inode *inode
){
        return sivfs_sb_to_shared_state(inode->i_sb);
}

/*
HEADER_INLINE struct sivfs_fs_info* sivfs_file_to_fs_info(struct file *file){
        asdf
}
*/

HEADER_INLINE struct sivfs_shared_state* sivfs_dentry_to_shared_state(
        struct dentry *dentry
){
        return sivfs_inode_to_shared_state(dentry->d_inode);
}

//Deletes all entries from the logs before stop_ts. Does not remove the entry
//at stop_ts.
//
//Writes true to did_delete if at least one entry was removed.
void sivfs_delete_log(bool* did_delete, struct sivfs_shared_state* sstate, sivfs_ts stop_ts);

//Loads a log entry, the return is guaranteed to have valid next and prev
//pointers from ts_start to ts_stop
//Should be called with rcu_read lock acquired
int sivfs_ts_to_log_entry(
        struct sivfs_log_entry** out_log,
        struct sivfs_shared_state* sstate,
        sivfs_ts ts,
        sivfs_ts ts_start,
        sivfs_ts ts_stop
);

//Prepares a log entry for a new commit
//Not thread-safe, must be called with some sort of lock held
// The created entry will be pre-allocated with a WS sufficiently large
// to hold log_writeset
int sivfs_create_log_entry(
        struct sivfs_log_entry** out_log,
        struct sivfs_shared_state* sstate,
        sivfs_ts ts,
        struct sivfs_log_entry* log_writeset
);


//Checks if writeset entry ent invalidates any part of file offset range [start, end).
//It is assumed end > start.
HEADER_INLINE bool sivfs_range_overlaps_wsent(
        size_t start,
        size_t end,
        struct sivfs_writeset_entry* ent
){
        //TODO this only handles the most basic writeset entries, i.e. not
        //streams
        size_t x = ent->file_offset;
        return x >= start && x < end;
}

/*
 * Scans the log for a write touching file ino range [offset_start, offset_end)
 * at timestamps (start_ts, end_ts] (so, if end_ts == start_ts, no match is
 * found)
 *
 * Returns SIVFS_INVALID_TS only if there is guaranteed no write, otherwise
 * returns the first timestamp (into first_modifier_out) where a might to the
 * range might have occurred.
 *
 * Used to determine whether checking out a part of a checkpoint is possible at some
 * timestamp.
 */
HEADER_INLINE int sivfs_find_first_write(
        struct sivfs_shared_state* sstate,
        sivfs_ts* first_modifier_out,
        sivfs_ts start_ts,
        sivfs_ts end_ts,
        unsigned long file_ino,
        size_t offset_start,
        size_t offset_end
){
        int rc = 0;

        if (end_ts < start_ts || offset_end <= offset_start){
                rc = -EINVAL;
                goto error0;
        }
        if (end_ts == start_ts){
                *first_modifier_out = SIVFS_INVALID_TS;
                goto out;
        }

        struct sivfs_log_entry* log_ent;
        rc = sivfs_ts_to_log_entry(
                &log_ent,
                sstate,
                start_ts+1,
                start_ts+1,
                end_ts
        );
        if (rc)
                goto error0;
        if (log_ent == NULL){
                dout("Assertion error");
                rc = -EINVAL;
                goto error0;
        }

        while(1){
                if (log_ent->aborted){
                        goto continue_loop;
                }

                //Convenience variable
                struct sivfs_writeset* ws = &log_ent->ws;

                //Apply all writeset entries in log_ent if they apply
                size_t i;
                struct sivfs_writeset_entry* ent = ws->entries;
                size_t size_left = ws->__size_bytes;
                while(size_left){
                        struct sivfs_wse_stream_info info;
                        rc = sivfs_wse_to_stream_info(&info, ent, size_left);
                        if (rc){
                                dout("Here");
                                goto error0;
                        }

                        if (ent->ino == file_ino &&
                        sivfs_range_overlaps_wsent(
                                offset_start,
                                offset_end,
                                ent
                        )){
                                //Found a hit
                                *first_modifier_out = log_ent->ts;
                                goto out;
                        }

                        //Advance
                        size_t wse_size = sivfs_wse_size(&info);
                        ent = (struct sivfs_writeset_entry*)(
                                ((char*)ent) + wse_size
                        );
                        size_left -= wse_size;
                }

                struct sivfs_log_entry* next_clog;

continue_loop:
                next_clog = log_ent->next;
                if (!next_clog || next_clog->ts > end_ts){
                        //With timestamps assigned to every integer, this
                        //never occurs, but may as well handle the more
                        //general case where we can non-sequential timestamps
                        break;
                }
                log_ent = next_clog;
        }

        //Made it through without finding a writer.
        *first_modifier_out = SIVFS_INVALID_TS;

out:
error0:
        return rc;
}

#endif
