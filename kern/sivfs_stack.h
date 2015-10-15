#ifndef SIVFS_STACK_H
#define SIVFS_STACK_H

#ifndef __KERNEL__
#define dout(x)
#else
#include <linux/slab.h>
#include <linux/vmalloc.h>
#endif

#include "sivfs_common.h"

//Implementation of a stack as a dynamic array
//Uses vmalloc to allocate the buffer, so allows both large and small stacks

//Zero initialization currently OK
struct sivfs_stack {
        size_t size;
        //value that size can reach before reallocating entries is necessary
        size_t capacity;
        void** values;
};

#ifdef __KERNEL__

HEADER_INLINE int sivfs_new_stack(struct sivfs_stack** new_stack){
        int rc = 0;

        struct sivfs_stack* stack;
        stack = kzalloc(sizeof(*stack), GFP_KERNEL);

        if (!stack){
                rc = -ENOMEM;
                goto error0;
        }

        *new_stack = stack;

out:
        return rc;

error0:
        return rc;
}

HEADER_INLINE void sivfs_destroy_stack(struct sivfs_stack* stack){
        vfree(stack->values);
}

//Used for freeing the result of new_stack
HEADER_INLINE void sivfs_put_stack(struct sivfs_stack* stack){
        if (unlikely(ZERO_OR_NULL_PTR(stack))){
                return;
        }
        sivfs_destroy_stack(stack);
        kfree(stack);
}

#else //ifndef__KERNEL__

HEADER_INLINE void sivfs_destroy_stack(struct sivfs_stack* stack){
        free(stack->values);
}

#endif

HEADER_INLINE int sivfs_stack_empty(struct sivfs_stack* stack){
       return stack->size == 0;
}


//Tries to reserve enough space for new_capacity. If no memory,
//stack is unmodified and an error is returned.
HEADER_INLINE int sivfs_stack_reserve(
        struct sivfs_stack* stack,
        size_t new_capacity
){
        int rc = 0;
        if (new_capacity >= stack->capacity){
                //reallocate
                new_capacity = max3_size_t(
                        stack->capacity * 2,
                        4,
                        new_capacity
                );
                void** newalloc;
#ifdef __KERNEL__
                newalloc = vzalloc(
                        new_capacity * sizeof(*stack->values)
                );
                if (!newalloc){
                        rc = -ENOMEM;
                        goto error0;
                }
                void** old_values = stack->values;
                memcpy(
                        newalloc,
                        stack->values,
                        sizeof(*stack->values) * stack->size
                );

                //Hmm. Use object cache here?
                vfree(old_values);
#else //ifdef __KERNEL__
                newalloc = reinterpret_cast<decltype(newalloc)>( realloc(
                        stack->values,
                        new_capacity * sizeof(*stack->values)
                ));
                if (!newalloc){
                        rc = -ENOMEM;
                        goto error0;
                }
#endif //ifndef __KERNEL__
                stack->capacity = new_capacity;
                stack->values = newalloc;
        }

error0:
        return rc;
}

HEADER_INLINE int sivfs_stack_push(struct sivfs_stack* stack, void* value){
        int rc = 0;
        size_t new_size = stack->size + 1;
        rc = sivfs_stack_reserve(stack, new_size);
        if (rc)
                goto error0;
        //Set new size and do struct assignment
        stack->values[stack->size] = value;
        stack->size = new_size;

error0:
        return rc;
}

//Pushes all elements in stack_src onto stack_dest.
//stack_src is unmodified.
//If an error occurs, no elements are pushed onto dest.
//
//Note that in no cases is stack_src modified.
HEADER_INLINE int sivfs_stack_push_all(
        struct sivfs_stack* stack_dest,
        const struct sivfs_stack* stack_src
){
        int rc = 0;
        size_t i, nadd, new_size;

        //Sanity checks...
        if (!stack_dest){
                dout("Sanity check failed");
                rc = -EINVAL;
                goto error0;
        }
        if (!stack_src){
                dout("Sanity check failed");
                rc = -EINVAL;
                goto error0;
        }

        nadd = stack_src->size;
        new_size = stack_dest->size + nadd;
        rc = sivfs_stack_reserve(stack_dest, new_size);
        if (rc)
                goto error0;

        //The next assignments will not fail now.
        i = 0;
        for(i = 0; i < nadd; i++){
                stack_dest->values[stack_dest->size] = stack_src->values[i];
                stack_dest->size++;
        }

error0:
        return rc;
}

//Same as pop except don't actually remove the element.
HEADER_INLINE void* sivfs_stack_peek(struct sivfs_stack* stack){
        if (sivfs_stack_empty(stack)){
                dout("WARNING: Attmpt to peek an empty stack!");
                return NULL;
        }
        void* toRet = stack->values[stack->size-1];
        return toRet;
}

HEADER_INLINE void* sivfs_stack_pop(struct sivfs_stack* stack){
        if (sivfs_stack_empty(stack)){
                dout("WARNING: Attmpt to pop an empty stack!");
                return NULL;
        }
        void* toRet = stack->values[stack->size-1];
        stack->size--;
        return toRet;
}

//O(N) removal by index
HEADER_INLINE void sivfs_stack_remove(
        struct sivfs_stack* stack,
        size_t i
){
        if (i >= stack->size){
                dout("WARNING: Attempt to remove past-the-end of stack!");
                return;
        }

        size_t trailing = stack->size - i - 1;
        memcpy(
                stack->values + i,
                stack->values + i + 1,
                trailing * sizeof(*stack->values)
        );
        stack->size--;
}

#endif
