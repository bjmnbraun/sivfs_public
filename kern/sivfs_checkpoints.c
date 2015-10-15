#include <linux/pagemap.h>
#include <linux/highmem.h>

#include "sivfs_checkpoints.h"
#include "sivfs_radix_tree.h"
#include "sivfs_stack.h"
#include "sivfs_mm.h"
#include "sivfs.h"
#include "sivfs_state.h"

int sivfs_get_zero_checkpoint_pages(void);
void sivfs_put_zero_checkpoint_pages(void);

int sivfs_init_checkpoints(struct sivfs_checkpoints* checkpoints){
        int rc = 0;

        //Init should begin with a zeroing for safety
        memset(checkpoints, 0, sizeof(*checkpoints));
        mutex_init(&checkpoints->checkpoints_reverse_mutex);
        INIT_RADIX_TREE(&checkpoints->checkpoints_reverse, GFP_KERNEL);
        INIT_RADIX_TREE(&checkpoints->gc_blocked_entries, GFP_KERNEL);
        mutex_init(&checkpoints->gc_blocked_entries_mutex);
        spin_lock_init(&checkpoints->obsoleted_entries_lock);

        //Initialize / refcount bump a cache of zero checkpoint pages
        rc = sivfs_get_zero_checkpoint_pages();
error0:
        return rc;
}

struct address_space _checkpoint_page_mapping = {
        .a_ops = &sivfs_aops
};

//For checkpoint pages where we have put the pginfo somewhere else
struct address_space _checkpoint_page_noprivate_mapping = {
        .a_ops = &sivfs_aops
};

atomic64_t sivfs_ncheckpoint_pages;

int sivfs_new_checkpoint_page(
        struct page** page_out,
        sivfs_ts checkpoint_ts,
        size_t level
){
        int rc = 0;

        //TODO possibly something throughh the cgroups system will let us set
        //up accounting for checkpoint page allocation?
        //
        //Speculative increment. The counter may briefly indicate that we have
        //overstepped the limit, but we never actually do.
        //
        //TODO actually for the CHECKOUT_CPS hack we allocate a bit more than
        //PGSIZE so this doesn't properly account for that.
        long _sivfs_ncheckpoint_pages = atomic64_inc_return(&sivfs_ncheckpoint_pages);
        if (_sivfs_ncheckpoint_pages > SIVFS_MAX_CHECKPOINT_PAGES){
                rc = -ENOMEM;
                goto error0;
        }

        //what order page new_page is
        unsigned int order;
        struct page* new_page;
        struct sivfs_checkpoint_pginfo* pginfo;
        if ((level == 1 || level == 2) &&
         (SIVFS_FIGURE_CHECKOUT_CPS == SIVFS_FIGURE_YES_CHECKOUT_CPS)){
                //Yuck - pmd page ctor / page ctor are overwriting the low bits of
                //private! Level 1 and 2 pages need a workaround where we put the
                //private some place else. Luckily, it doesn't need to be fast to get
                //that information since since the fault path ignores the cp_info (it
                //only contains information useful for making new checkpoints / garbage
                //collection.) And, there are many more leaf pages than parents.
                //
                //An alternative way is to have each non-leaf node is an order-1 page
                //where the first page is as usual but the second page is a page of
                //pointers-to-pginfo of the children. This convention of
                //adding-1-to-page to get the pginfo page won't disrupt the hardware.
                //Also, it's actually more efficient to get pginfo that way, as opposed
                //to following the pginfo pointer of the page.
                //Put the pginfo someplace other than private - we need
                //private!
                order = 1;
                new_page = alloc_pages(GFP_KERNEL, order);
                if (!new_page){
                        dout("Here");
                        rc = -ENOMEM;
                        goto error0;
                }
                pginfo = sivfs_checkpoint_page_to_pginfo_noprivate(new_page);
                if (!pginfo){
                        dout("Assertion error");
                        rc = -EINVAL;
                        goto error1;
                }
                //Zero out pginfo
                memset(pginfo, 0, sizeof(*pginfo));
                pginfo->is_noprivate = true;
                new_page->mapping = &_checkpoint_page_noprivate_mapping;
                //We need to set Page Dirty to prevent reclaim
                SetPageDirty(new_page);
        } else {
                order = 0;
                new_page = alloc_page(GFP_KERNEL);
                if (!new_page){
                        rc = -ENOMEM;
                        goto error0;
                }
                //Allocate a pginfo
                pginfo = kzalloc(sizeof(*pginfo), GFP_KERNEL);
                if (!pginfo){
                        rc = -ENOMEM;
                        goto error1;
                }
                //Put the pginfo in private.
                SetPagePrivate(new_page);
                set_page_private(new_page, (unsigned long) pginfo);
                new_page->mapping = &_checkpoint_page_mapping;
        }

        if (PageHighMem(new_page)){
                dout("Assertion error.");
                goto error2;
        }

        //clear for safety
        //memset(pginfo, 0, sizeof(*pginfo));
        pginfo->checkpoint_ts = checkpoint_ts;
        pginfo->obsoleted_ts = SIVFS_INVALID_TS;
        pginfo->level = level;
        pginfo->order = order;

        if (SIVFS_FIGURE_CHECKOUT_CPS == SIVFS_FIGURE_YES_CHECKOUT_CPS){
                if (level == 2){
                        pginfo->is_pmd = true;
                        if (!pgtable_pmd_page_ctor(new_page)){
                                rc = -ENOMEM;
                                goto error2;
                        }
                } else if (level == 1){
                        pginfo->is_pte = true;
                        if (!pgtable_page_ctor(new_page)){
                                rc = -ENOMEM;
                                goto error2;
                        }
                }

                /*
                //Yuck - we don't account mapcount on checkpoint pages so that we can
                //move around subtrees of pages
                atomic_add(1ULL<<29, &new_page->_mapcount);
                //This functgion is not exported on 4_2_1!
                set_page_count(new_page, 1ULL<<29);
                */
        }

        //Set the page up to date
        SetPageUptodate(new_page);

        struct sivfs_checkpoint_pginfo* pginfo_check = sivfs_checkpoint_page_to_pginfo(
                new_page
        );
        if (pginfo_check != pginfo){
                dout("Assertion error");
                rc = -EINVAL;
                goto error3;

        }

out:
        *page_out = new_page;
        return rc;

error3:
        if (pginfo->is_pmd){
                pgtable_pmd_page_dtor(new_page);
        }
        if (pginfo->is_pte){
                pgtable_page_dtor(new_page);
        }

error2:
        if (pginfo->is_noprivate){
                //Nothing to do
        } else {
                kfree(pginfo);
                ClearPagePrivate(new_page);
        }
        new_page->mapping = NULL;

error1:
        __free_pages(new_page, order);

error0:
        //Undo speculative increment
        atomic64_dec(&sivfs_ncheckpoint_pages);
        return rc;
}

//Page gets references as it is mapped into thread's mmaps. However,
//this is the final put call that is only called when it is not referenced
//by any thread's mmaps, so it must have refct == 1.
void sivfs_put_checkpoint_page(struct page* page){
        if (SIVFS_FIGURE_CHECKOUT_CPS == SIVFS_FIGURE_YES_CHECKOUT_CPS){
                //Yuck! _mapcount starts at -1!
                /*
                atomic_set(&page->_mapcount, -1);
                set_page_count(page, 1);
                */
        }

        struct sivfs_checkpoint_pginfo* pginfo =
                sivfs_checkpoint_page_to_pginfo(page)
        ;
        if (!pginfo){
                WARN_ONCE(true, "WARN: Checkpoint page had no pginfo. Might not be properly cleaned.");
                return;
        }

        //Just a check:
        int refct = page_ref_count(page);
        int mapct = page_mapcount(page);
        if (refct != 1 || mapct != 0){
                dout(
                        "WARN: Bad page refct / mapcount! %d %d. Page may not be cleaned."
                        " Level %zd, %p, [%llu, %llu)"
                        ,
                        refct,
                        mapct,
                        pginfo->level,
                        page,
                        pginfo->checkpoint_ts,
                        pginfo->obsoleted_ts
                );
                return;
        }

        if (mapct != 0){
                dout("WARN: Bad page mapct! %d. Page may not be cleaned.", mapct);
                return;
        }

        if (PageLocked(page)){
                dout("WARN: Page was locked! %p. Page may not be cleaned.", page);
                return;
        }

        //Pull the order off
        unsigned int order = pginfo->order;

        atomic64_dec(&sivfs_ncheckpoint_pages);
        if (pginfo->is_pmd){
                pgtable_pmd_page_dtor(page);
        }
        if (pginfo->is_pte){
                pgtable_page_dtor(page);
        }
        //For noprivate pages, the pginfo is stored in the pages themselves so
        //don't free pginfo / clear private:
        if (pginfo->is_noprivate){
                ClearPageDirty(page);
        } else {
                kfree(pginfo);
                ClearPagePrivate(page);
        }

        //We should be the last to free the page. So we can set mapping
        //to NULL safely here.
        page->mapping = NULL;

        __free_pages(page, order);
}

struct sivfs_checkpoint_pginfo* sivfs_checkpoint_page_to_pginfo_noprivate(
        struct page* page
){
        return (struct sivfs_checkpoint_pginfo*)(
                (char*)page_address(page) + PAGE_SIZE
        );
}

//Recursively frees a snapshot and all visible pages. Returns number of pages
//thus freed.
size_t __sivfs_checkpoint_root_free(struct page* root, size_t level);

HEADER_INLINE size_t _sivfs_checkpoint_root_free(struct page* root){
        return __sivfs_checkpoint_root_free(root, SIVFS_CHECKPOINT_TOTAL_LEVELS - 1);
}

void sivfs_destroy_checkpoints(
        struct sivfs_checkpoints* checkpoints
){
        //At this point, we have:
        //Exactly one most recent snapshot, where all pages visible from it do
        //not have an "end" timestamp, and hence are not in the
        //gc_blocked_entries tree
        //
        //and all other snapshot pages, which are in the gc_blocked_entries tree
        //
        //This is in fact true - because the gc_blocked_entries tree always has
        //an entry at SIVFS_INVALID_TS which contains pages obsoleted but which
        //the gc thread hasn't gotten around to dealing with / recategorizing.
        //

        //Convenience
        struct sivfs_latest_checkpoint* _latest_checkpoint =
                &checkpoints->latest_checkpoint
        ;
        //Don't need to do double-read-lock here since we are cleaning up, and
        //hence all threads touching checkpoints have exited
        struct inode* latest_checkpoint = _latest_checkpoint->checkpoint;

        if (latest_checkpoint){
                struct sivfs_inode_info* cpinfo =
                        sivfs_inode_to_iinfo(latest_checkpoint)
                ;

                dout(
                        "Freeing latest checkpoint @ %llu",
                        cpinfo->cp_ts
                );

                //if this misbehaves, hard to detect. Some best effort tests
                //follow.
                size_t numFreed = _sivfs_checkpoint_root_free(
                        cpinfo->cp_root
                );
                if (cpinfo->cp_root && !numFreed){
                        dout("ERROR: Did not free any pages of latest checkpoint");
                }
                //Whatever
                if (!cpinfo->cp_root && numFreed){
                        dout("ERROR: Freed pages without checkpoint (?)");
                }
                iput(latest_checkpoint);
        }

        //Remove all entries from checkpoints_reverse
        //
        //TODO get rid of this tree - with how we garbage collect it is
        //pointing to freed inodes all the time!
        //
        //What we could do instead is a have a tree of "archive" checkpoints
        //that are kept and we would need to free those inodes here.
        //
        //Pages visible by any archive checkpoints should not be reclaimed in
        //GC. We pass created checkpoints to a "checkpoint notifier" that can
        //choose to create an archive reference to a checkpoint. The notifier
        //can also register the checkpoint inode with some dentry.
        //
        //not incredibly sexy but would be a good unit test to verify
        //historical queries work properly
        {
                void** slot;
                struct radix_tree_iter iter;
                unsigned long indices[16];
                int i, nr;
                unsigned long index;
                //Acc to gmap_radix_tree_free, the stuff below should work.
                do {
                        nr = 0;
                        radix_tree_for_each_slot(
                                slot,
                                &checkpoints->checkpoints_reverse,
                                &iter,
                                0
                        ){
                                /*
                                struct inode* checkpoint = *slot;
                                iput(checkpoint);
                                */

                                indices[nr] = iter.index;
                                if (++nr == 16)
                                        break;
                        }
                        for(i = 0; i < nr; i++){
                                index = indices[i];
                                radix_tree_delete(
                                        &checkpoints->checkpoints_reverse,
                                        index
                                );
                        }
                } while (nr > 0);
        }

        if (!sivfs_radix_tree_empty(&checkpoints->checkpoints_reverse)){
                dout("ERROR: Did not get rid of all checkpoints!");
        }

        //Clean up gc_blocked_entries
        {
                void** slot;
                struct radix_tree_iter iter;
                unsigned long indices[16];
                int i, nr;
                unsigned long index;
                //Acc to gmap_radix_tree_free, the stuff below should work.
                do {
                        nr = 0;
                        radix_tree_for_each_slot(
                                slot,
                                &checkpoints->gc_blocked_entries,
                                &iter,
                                0
                        ){
                                struct sivfs_stack* blocked_entries = *slot;
                                for(i = 0; i < blocked_entries->size; i++){
                                        sivfs_gc_entry* entry =
                                                (sivfs_gc_entry*)
                                                &blocked_entries->values[i]
                                        ;
                                        sivfs_gc_free_entry(entry, NULL);
                                }
                                sivfs_put_stack(blocked_entries);

                                indices[nr] = iter.index;
                                if (++nr == 16)
                                        break;
                        }
                        for(i = 0; i < nr; i++){
                                index = indices[i];
                                radix_tree_delete(
                                        &checkpoints->gc_blocked_entries,
                                        index
                                );
                        }
                } while (nr > 0);
        }

        if (!sivfs_radix_tree_empty(&checkpoints->gc_blocked_entries)){
                dout("ERROR: Deleting checkpoints with non-gc'ed blocked pages!");
        }

        //Clean up obsoleted_entries;
        {
                struct sivfs_stack* obsoleted_entries =
                        &checkpoints->obsoleted_entries
                ;
                size_t i;
                for(i = 0; i < obsoleted_entries->size; i++){
                        sivfs_gc_entry* entry =
                                (sivfs_gc_entry*)
                                &obsoleted_entries->values[i]
                        ;
                        sivfs_gc_free_entry(entry, NULL);
                }
                sivfs_destroy_stack(obsoleted_entries);
        }

        mutex_destroy(&checkpoints->checkpoints_reverse_mutex);
        mutex_destroy(&checkpoints->gc_blocked_entries_mutex);

        sivfs_put_zero_checkpoint_pages();

        uint64_t ncheckpoint_pages = atomic64_read(&sivfs_ncheckpoint_pages);
        if (ncheckpoint_pages){
                dout(
                        "ERROR: Closing sivfs with %llu unfreed checkpoint pages!",
                        ncheckpoint_pages
                );
        }
}

HEADER_INLINE unsigned long sivfs_checkpoints_ts_to_reverse_index(sivfs_ts ts){
        //Needs to map 0 to the largest possible value, and then
        //decrease with increasing ts
        //
        //Hence, an increasing search from this index will result in
        //the next _oldest_ snapshot
        return ((unsigned long)-1) + (-ts);
}

HEADER_INLINE sivfs_ts sivfs_checkpoints_reverse_index_to_ts(unsigned long index){
        //Needs to invert above
        return -(index + 1);
}

void sivfs_checkpoint_lookup(
        struct inode** checkpoint_out,
        struct sivfs_checkpoints* checkpoints,
        sivfs_ts desired_ts
) {
        //need to do a traversal to find next element
        //forward iteration goes back in time because we use -ts as the index
        //in checkpoints_reverse tree
        struct inode* checkpoint = NULL;
        unsigned long found_reverse_index = 0;
        bool found = false;

        //Not allowed to look at any unpublished entries.
        sivfs_ts latest_snapshot =
                atomic64_read(&checkpoints->latest_checkpoint_published)
        ;
        if (desired_ts > latest_snapshot){
                desired_ts = latest_snapshot;
        }

        mutex_lock(&checkpoints->checkpoints_reverse_mutex);
        void** slot;
        struct radix_tree_iter iter;
        radix_tree_for_each_slot(
                slot,
                &checkpoints->checkpoints_reverse,
                &iter,
                sivfs_checkpoints_ts_to_reverse_index(desired_ts)
        ){
                found = true;
                found_reverse_index = iter.index;
                checkpoint = *slot;
                //Break immediately on first hit. Omitting this is a serious
                //bug.
                //Hack using if (true) to trick -Wunreachable-code
                if (true) break;
        }

        *checkpoint_out = checkpoint;
        mutex_unlock(&checkpoints->checkpoints_reverse_mutex);
}

//Really just does the first half of insertion, inserting an entry into the
//radix tree. The checkpoint is not yet visible.
//Returns -EEXIST if there is already a snapshot
//at a particular index
//this is a necessary sanity check if we start doing things like
//index = ts / 100 to make sure we don't overwrite old checkpoints that
//alias to the same index
int _sivfs_checkpoints_insert(
        struct sivfs_checkpoints* checkpoints,
        struct inode* checkpoint
){
        int rc = 0;
        if (!checkpoint){
                //The way linux radix trees work is they store NULL for empty
                //slots => we can't store NULL
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_inode_info *cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;

        rc = mutex_lock_interruptible(&checkpoints->checkpoints_reverse_mutex);
        if (rc){
                dout("Here");
                goto error0;
        }
        rc = radix_tree_insert(
                &checkpoints->checkpoints_reverse,
                sivfs_checkpoints_ts_to_reverse_index(cpinfo->cp_ts),
                checkpoint
        );
        mutex_unlock(&checkpoints->checkpoints_reverse_mutex);

out:

error0:
        return rc;
};

//Publishes the checkpoint, previously inserted with
//_sivfs_checkpoints_insert
void _sivfs_checkpoints_insert_finish(
        struct sivfs_checkpoints* checkpoints,
        struct inode* checkpoint
){
        struct sivfs_inode_info *cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;

        sivfs_ts cp_ts = cpinfo->cp_ts;

        //Double-read-lock set latest_cp
        struct sivfs_latest_checkpoint* latest_cp =
                &checkpoints->latest_checkpoint
        ;
        atomic64_set(&latest_cp->ts, SIVFS_INVALID_TS);
        latest_cp->checkpoint = checkpoint;
        atomic64_set(&latest_cp->ts, cp_ts);

        //Finally, update latest_checkpoint_published after publishing
        atomic64_set(&checkpoints->latest_checkpoint_published, cp_ts);
}


void _sivfs_checkpoints_remove(
        struct sivfs_checkpoints* checkpoints,
        struct inode* checkpoint
){
        struct sivfs_inode_info* cp_info =
                sivfs_inode_to_iinfo(checkpoint)
        ;
        mutex_lock(&checkpoints->checkpoints_reverse_mutex);
        radix_tree_delete(
                &checkpoints->checkpoints_reverse,
                sivfs_checkpoints_ts_to_reverse_index(cp_info->cp_ts)
        );
        mutex_unlock(&checkpoints->checkpoints_reverse_mutex);
}

//@level_stop - stop traversing at this level (0 to traverse all the way
//to the leaf node and return it.)
static int sivfs_checkpoint_traverse_partial(
        struct page** checkpoint_page_out,
        struct page* checkpoint_root,
        unsigned long file_ino,
        size_t file_offset,
        int level_stop
) {
        int rc = 0;

        struct sivfs_checkpoint_iter iter;
        rc = sivfs_checkpoint_iter_init(
                &iter,
                file_ino,
                file_offset
        );
        if (rc)
                goto error0;

        phys_addr_t pa = 0;
        if (checkpoint_root){
                pa = page_to_pa(checkpoint_root);
        }

        size_t level;
        for(level = SIVFS_CHECKPOINT_TOTAL_LEVELS - 1; ;){
                //Advance?
                if (level == level_stop){
                        break;
                }

                if (!pa){
                        //Can't advance, parent missing
                        *checkpoint_page_out = NULL;
                        goto out;
                }

                //Otherwise, continue down the tree
                PxD_ent* table = __va(pa);
                size_t offset = sivfs_checkpoint_iter_next_offset(&iter);
                phys_addr_t next_phys = sivfs_pxd_to_pa(table[offset]);
                sivfs_checkpoint_iter_advance(&iter);
                pa = next_phys;
                level--;
        }

        if (pa){
                //We successfully found the page.
                *checkpoint_page_out = pa_to_page(pa);
        } else {
                //Target page didn't exist
                *checkpoint_page_out = NULL;
        }

out:
error0:
        return rc;
}

int sivfs_checkpoint_traverse(
        struct page** checkpoint_page_out,
        struct page* checkpoint_root,
        unsigned long file_ino,
        size_t file_offset
) {
        return sivfs_checkpoint_traverse_partial(
                checkpoint_page_out,
                checkpoint_root,
                file_ino,
                file_offset,
                0
        );
}

//node is an internal node of a snapshot at level level. We recurse down the
//tree, i.e. with decreasing levels
//Returns number of pages freed
size_t __sivfs_checkpoint_root_free(struct page* node, size_t level){
        size_t toRet = 0;
        if (!node){
                goto out;
        }

        if (!sivfs_is_checkpoint_page(node)){
                dout(
                        "ERROR! Non-checkpoint page in cp: %016llx %d",
                        page_to_pa(node),
                        page_ref_count(node)
                );
                goto out;
        }

        //Zero pages are reference not reference tracked
        if (sivfs_is_zero_page(node)){
                goto out;
        }

        if (level == 0){
                //Leaf node.
                goto free_node;
        }

        //Otherwise, we have children so recurse:
        PxD_ent* table = (PxD_ent*)page_address(node);
        size_t i;
        for(i = 0; i < SIVFS_CHECKPOINT_CHILDREN_PER_NODE; i++){
                //Go through non-null children that are not zero pages
                if (sivfs_pxd_present(table[i])){
                        struct page* child = pa_to_page(
                                sivfs_pxd_to_pa(table[i])
                        );
                        if (!sivfs_is_checkpoint_page(child)){
                                dout(
                                        "ERROR! Non-checkpoint page in cp: %016llx %d",
                                        page_to_pa(child),
                                        page_ref_count(child)
                                );
                                continue;
                        }
                        if (sivfs_is_zero_page(child)){
                                continue;
                        }
                        if (level == 1){
                                //Page table can just shortcut
                                sivfs_put_checkpoint_page(child);
                                toRet++;
                        } else {
                                //Internal node.
                                toRet += __sivfs_checkpoint_root_free(child, level-1);
                        }
                }
        }

free_node:
        //Finally, free node
        sivfs_put_checkpoint_page(node);
        toRet++;

out:
        return toRet;
}

//TODO if we introduce holes in checkpoints this will change
int sivfs_read_checkpoint(
        void* mem,
        size_t mem_size,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset
){
        int rc = 0;

        if ((file_offset % PAGE_SIZE) || (mem_size != PAGE_SIZE)){
                //Not yet supported
                rc = -EINVAL;
                goto error0;
        }

        if (!checkpoint){
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;

        struct page* cp_page;
        rc = sivfs_checkpoint_traverse(
                &cp_page,
                cpinfo->cp_root,
                file_ino,
                file_offset
        );
        if (rc)
                goto error0;

        if (cp_page){
                void* cp_mem = page_address(cp_page);
                memcpy(mem, cp_mem, mem_size);
        } else {
                //Fill with zeros
                memset(mem, 0, mem_size);
        }

error0:
        return rc;
}

//TODO if we introduce holes in checkpoints this will change
int sivfs_lock_cp_page(
        struct page** page_out,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset,
        unsigned int flags
){
        int ret = 0;
        int rc = 0;

        if (!checkpoint){
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;
        struct page* cp_page;
        rc = sivfs_checkpoint_traverse(
                &cp_page,
                cpinfo->cp_root,
                file_ino,
                file_offset
        );
        if (rc) {
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        if (!cp_page){
                //No such page.
                *page_out = NULL;
                goto out;
        }

        get_page(cp_page);
        lock_page(cp_page);

#if 0
        //For some reason lock_page_killable is causing problems. Avoid it for
        //now.
        //
        //Also, though rarely, under memory constrained resources we are
        //occasionally called with flags that don't let us retry. So just never
        //do this.
        if (!trylock_page(cp_page) && !lock_page_killable(cp_page)){
                dout("Couldn't lock CP page!");
                put_page(cp_page);
                ret |= VM_FAULT_RETRY;
                //ret = VM_FAULT_SIGBUS;
                goto out;
        }
#endif

        //All right we can serve this page
        *page_out = cp_page;
        ret = VM_FAULT_LOCKED;

out:
error0:
        return ret;
}

//TODO if we introduce holes in checkpoints this will change
int sivfs_get_cp_page(
        struct page** page_out,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset
){
        int ret = 0;
        int rc = 0;

        if (!checkpoint){
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;
        struct page* cp_page;
        rc = sivfs_checkpoint_traverse(
                &cp_page,
                cpinfo->cp_root,
                file_ino,
                file_offset
        );
        if (rc) {
                ret = VM_FAULT_SIGBUS;
                goto error0;
        }

        if (!cp_page){
                //No such page.
                *page_out = NULL;
                goto out;
        }

        dout("Current page refct is %d\n", page_ref_count(cp_page));
        get_page(cp_page);

        //All right we can serve this page
        *page_out = cp_page;

out:
error0:
        return ret;
}

int sivfs_lookup_cp_page_at_level(
        struct page** page_out,
        struct inode* checkpoint,
        unsigned long file_ino,
        size_t file_offset,
        int level_stop
){
        int rc = 0;

        if (!checkpoint){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        //Sanity checking - level must be an FOFFSET-indirecting level
        //(there is currently no use case for higher level lookups)
        if (level_stop > SIVFS_CHECKPOINT_FOFFSET_LEVELS){
                dout("Here");
                rc = -EINVAL;
                goto error0;
        }

        struct sivfs_inode_info* cpinfo =
                sivfs_inode_to_iinfo(checkpoint)
        ;
        struct page* cp_page;
        rc = sivfs_checkpoint_traverse_partial(
                &cp_page,
                cpinfo->cp_root,
                file_ino,
                file_offset,
                level_stop
        );
        if (rc){
                dout("Here");
                goto error0;
        }

out:
        *page_out = cp_page;

error0:
        return rc;
}


//See sivfs_init_zero_checkpoint_pages
//ZERO_PAGE has special handling in that mkwrite on ZERO_PAGE generates a fault
//rather than trying to call do_mkwrite with (messed up) arguments since it
//won't have the pgoff = index set correctly.

//Acquire this when creating / destroying zero_pxd_page array
static DEFINE_SPINLOCK(sivfs_zero_pages_lock);
int64_t sivfs_zero_pages_refcount = 0;
struct page* sivfs_zero_pxd_page [SIVFS_CHECKPOINT_FOFFSET_LEVELS] = {NULL};
struct page* sivfs_zero_page;

//Zeros out a checkpoint page at some level in the checkpoint
// - level 0 pages -> simple zero (data pages start with 0s)
// - page table pages -> filled in so if the node can be mapped into
//   an address space where all accesses to the covered virtual memory region
//   point to a read-only zero page
// - higher level pages -> zeroed out, i.e. no children
void sivfs_mk_zero_checkpoint_page(void* mem, size_t level){
        if (level == 0){
                memset(mem, 0, PAGE_SIZE);
                return;
        }

        //Otherwise we are filling in a table of child references
        PxD_ent* table = mem;
        PxD_ent ent_to_fill;
        if (level == 1){
                ent_to_fill = sivfs_mk_pte(page_to_pa(sivfs_zero_page));
        } else if (level <= SIVFS_CHECKPOINT_FOFFSET_LEVELS){
                ent_to_fill = sivfs_mk_pxd_ent(page_to_pa(sivfs_zero_pxd_page[level-1]));
        } else {
                ent_to_fill = 0;
        }

        size_t i;
        for(i = 0; i < SIVFS_CHECKPOINT_CHILDREN_PER_NODE; i++){
                table[i] = ent_to_fill;
        }
}

static void _sivfs_destroy_zero_checkpoint_pages(void){
        size_t level;
        for(level = 1; level < SIVFS_CHECKPOINT_FOFFSET_LEVELS; level++){
                if (sivfs_zero_pxd_page[level]){
                        sivfs_put_checkpoint_page(sivfs_zero_pxd_page[level]);
                }
        }
        //Don't free sivfs_zero_page since it is equal to ZERO_PAGE(addr) for
        //some addr
}

static int _sivfs_init_zero_checkpoint_pages(void){
        int rc = 0;
        //Repeatedly call mk_zero_checkpoint_page to build up from 0
        size_t level;

        level = 0;
        //0 level just use ZERO_PAGE(0)
        sivfs_zero_pxd_page[level] = sivfs_zero_page = ZERO_PAGE(0);

        for(level = 1; level < SIVFS_CHECKPOINT_FOFFSET_LEVELS; level++){
                struct page* new_page;
                //Build a checkpoint page at time 0
                rc = sivfs_new_checkpoint_page(
                        &new_page,
                        0,
                        level
                );
                if (rc){
                        dout("Here");
                        goto error0;
                }
                struct sivfs_checkpoint_pginfo* pginfo =
                        sivfs_checkpoint_page_to_pginfo(new_page)
                ;
                pginfo->is_zero_page = true;
                sivfs_zero_pxd_page[level] = new_page;
                //Fill it in
                void* mem = page_address(new_page);
                if (!mem){
                        dout("Assertion error");
                        rc = -EINVAL;
                        goto error0;
                }
                sivfs_mk_zero_checkpoint_page(mem, level);
                __SetPageUptodate(new_page);

                dout("Zero checkpoint page %016llx %zd", page_to_pa(new_page), level);
        }

out:
        return rc;

error0:
        //Will free any pages we allocated
        _sivfs_destroy_zero_checkpoint_pages();
        return rc;
}

int sivfs_get_zero_checkpoint_pages(void){
        int toRet = 0;
        spin_lock(&sivfs_zero_pages_lock);
        if (!sivfs_zero_pages_refcount){
                //Try to create:
                toRet = _sivfs_init_zero_checkpoint_pages();
                if (toRet){
                        goto error0;
                }
        }
out:
        sivfs_zero_pages_refcount++;
error0:
        spin_unlock(&sivfs_zero_pages_lock);
        return toRet;
}

void sivfs_put_zero_checkpoint_pages(void){
        spin_lock(&sivfs_zero_pages_lock);
        if (sivfs_zero_pages_refcount <= 0){
                dout("Bad refcount. Skipping destruction.");
                goto error0;
        }
        sivfs_zero_pages_refcount--;
        if (sivfs_zero_pages_refcount == 0){
                _sivfs_destroy_zero_checkpoint_pages();
        }
error0:
        spin_unlock(&sivfs_zero_pages_lock);
}

void sivfs_gc_free_entry(
        sivfs_gc_entry* entry,
        struct sivfs_state* state
){
        int rc = 0;
        unsigned long type;
        rc = sivfs_gc_entry_to_type(&type, entry);
        if (rc){
                dout("ERROR: Bad type on gc entry, skipping free");
                return;
        }

        switch(type){
        case SIVFS_GC_ENTRY_PAGE:
                if (state) state->stats.nCheckpointPagesFreed++;

                sivfs_put_checkpoint_page(sivfs_gc_entry_to_page(entry));
                break;
        case SIVFS_GC_ENTRY_INODE:
                if (state) state->stats.nCheckpointInodesFreed++;

                iput(sivfs_gc_entry_to_inode(entry));
                break;
        }
}


