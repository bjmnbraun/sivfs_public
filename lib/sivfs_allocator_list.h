#ifndef SIVFS_ALLOCATOR_LIST
#define SIVFS_ALLOCATOR_LIST

//Just a simple list datastructure

//list node struct, should be a member of the elements in the list
struct sivfs_allocator_lnode {
        struct sivfs_allocator_lnode* prev;
        struct sivfs_allocator_lnode* next;
};

//Does not include "head" during iteration.
#define sivfs_alloc_list_for_each(pos, head) \
        for (pos = (head)->next; pos != (head); pos = pos->next)

//Does not count head as an element
HEADER_INLINE bool sivfs_alloc_list_empty(struct sivfs_allocator_lnode* head){
        return head->next == head;
}

HEADER_INLINE struct sivfs_allocator_lnode* sivfs_alloc_list_advance_incl(
        struct sivfs_allocator_lnode* iter,
        struct sivfs_allocator_lnode* head
){
        if (iter == head){
                return NULL;
        }
        return iter->next;
}
//Includes head during iteration. Head is the last element iterated over.
#define sivfs_alloc_list_for_each_incl(pos, head) \
        for (pos = (head)->next; pos; pos = sivfs_alloc_list_advance_incl(pos,(head)))

//Only used for initializing the head element, by making it a properly circular
//list. For list nodes intended to be added to some existing list, just
//initialize to 0s.
#define sivfs_init_alloc_list_head(head) {(head)->next = (head)->prev = (head);}

HEADER_INLINE void sivfs_alloc_list_add(
        struct sivfs_allocator_lnode* head,
        struct sivfs_allocator_lnode* toAdd
){
        //We will insert lh so it comes before target
        struct sivfs_allocator_lnode* target = head->next;
        //Equivalently, target_prev == head
        struct sivfs_allocator_lnode* target_prev = target->prev;

        if (toAdd->next || toAdd->prev){
                uout("ERROR - attempt to add list node already in a list");
                return;
        }

        toAdd->next = target;
        toAdd->prev = target_prev;
        target_prev->next = toAdd;
        target->prev = toAdd;
}

HEADER_INLINE void sivfs_alloc_list_remove(
        struct sivfs_allocator_lnode* toRemove
){
        struct sivfs_allocator_lnode* prev = toRemove->prev;
        struct sivfs_allocator_lnode* next = toRemove->next;

        if (toRemove == prev || toRemove == next){
                //This means we tried to remove a list header, which is not a
                //real element of the list
                uout("ERROR - attempt to remove list header. About to self destruct..");
                return;
        }

        toRemove->prev = NULL;
        toRemove->next = NULL;
        prev->next = next;
        next->prev = prev;
}


#endif
