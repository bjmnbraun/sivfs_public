#ifndef SIVFS_COMMIT_UPDATE_H
#define SIVFS_COMMIT_UPDATE_H

#include "sivfs_state.h"

//The following flags can be OR'ed together to form options to commit_update
//Abort the transaction.
#define SIVFS_ABORT_FLAG (1<<1)
//Update the latest checkpoint under direct-access, or fail with an error code
//if this is not possible.
#define SIVFS_DIRECT_ACCESS (1<<2)

//Using a generic framework for commit / update to allow for multiple
//implementations in different code files without having to do janky naming
struct sivfs_commit_update_operations {
        //If log_writeset is null or empty writeset, just updates.
        //Caller still owns log_writeset afterward (it is copied if used)
        //On success (0 return code), abort is set to true iff a transaction
        //was aborted
        //
        //See options above
        int (*commit_update) (
                bool* aborted,
                struct sivfs_state* state,
                struct sivfs_log_entry* log_writeset,
                int options
        );
        //Update PAGE_SIZE region of memory to the currently checked out version
        //Does not take advantage of the version currently stored in mem,
        //To update working set, use instead sivfs_apply_logs_workingset
        int (*update_page) (
                //Region to write into
                void* mem,
                struct sivfs_state* state,
                struct vm_area_struct* vma,
                //file offset, in bytes
                size_t file_offset
        );
};

extern const struct sivfs_commit_update_operations sivfs_commit_update_ops;

//Wrappers. Also sets stats that are independent of implementation.
HEADER_INLINE int sivfs_commit_update(
        bool* aborted,
        struct sivfs_state* state,
        struct sivfs_log_entry* log_writeset,
        int options
){

        if (SIVFS_DEBUG_COMMIT || SIVFS_DEBUG_UPDATE){
                dout("Commit starting.");
        }

        int rc = sivfs_commit_update_ops.commit_update(
                aborted,
                state,
                log_writeset,
                options
        );

        if (rc){
                dout("Here");
                goto error0;
        }

out:
        if (log_writeset && !sivfs_writeset_empty(&log_writeset->ws)){
                state->stats.nWriteTxns++;
        }

        state->stats.nTxns++;
        if (!*aborted){
                state->stats.nCommits++;
        }

error0:
        return rc;
}

HEADER_INLINE int sivfs_update_page(
        void* mem,
        struct sivfs_state* state,
        struct vm_area_struct* vma,
        size_t file_offset
){
        return sivfs_commit_update_ops.update_page(
                mem,
                state,
                vma,
                file_offset
        );
}

#endif
