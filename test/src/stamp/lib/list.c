/* =============================================================================
 *
 * list.c
 * -- Sorted singly linked list
 * -- Options: -DLIST_NO_DUPLICATES (default: allow duplicates)
 *
 * =============================================================================
 *
 * Copyright (C) Stanford University, 2006.  All Rights Reserved.
 * Author: Chi Cao Minh
 *
 * =============================================================================
 *
 * For the license of bayes/sort.h and bayes/sort.c, please see the header
 * of the files.
 *
 * ------------------------------------------------------------------------
 *
 * For the license of kmeans, please see kmeans/LICENSE.kmeans
 *
 * ------------------------------------------------------------------------
 *
 * For the license of ssca2, please see ssca2/COPYRIGHT
 *
 * ------------------------------------------------------------------------
 *
 * For the license of lib/mt19937ar.c and lib/mt19937ar.h, please see the
 * header of the files.
 *
 * ------------------------------------------------------------------------
 *
 * For the license of lib/rbtree.h and lib/rbtree.c, please see
 * lib/LEGALNOTICE.rbtree and lib/LICENSE.rbtree
 *
 * ------------------------------------------------------------------------
 *
 * Unless otherwise noted, the following license applies to STAMP files:
 *
 * Copyright (c) 2007, Stanford University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Stanford University nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY STANFORD UNIVERSITY ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL STANFORD UNIVERSITY BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * =============================================================================
 */


#include <stdlib.h>
#include <assert.h>
#include "list.h"
#include "types.h"
#include "tm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * DECLARATION OF TM_SAFE FUNCTIONS
 * =============================================================================
 */

TM_SAFE
list_node_t*
findPrevious (list_t* listPtr, void* dataPtr);

TM_SAFE
void
freeList (list_node_t* nodePtr);

TM_SAFE
list_node_t*
allocNode (void* dataPtr);

TM_SAFE
void
freeNode (list_node_t* nodePtr);

/* =============================================================================
 * compareDataPtrAddresses
 * -- Default compare function
 * =============================================================================
 */
TM_SAFE
long
compareDataPtrAddresses (const void* a, const void* b)
{
    return ((long)a - (long)b);
}


/* =============================================================================
 * list_iter_reset
 * =============================================================================
 */
TM_SAFE
void
list_iter_reset (list_iter_t* itPtr, list_t* listPtr)
{
    *itPtr = &(listPtr->head);
}


/* =============================================================================
 * list_iter_hasNext
 * =============================================================================
 */
// [wer210] removed the second junk parameter
TM_SAFE
bool_t
//list_iter_hasNext (list_iter_t* itPtr, list_t* listPtr)
list_iter_hasNext (list_iter_t* itPtr)
{
    return (((*itPtr)->nextPtr != NULL) ? TRUE : FALSE);
}

/* =============================================================================
 * list_iter_next
 * =============================================================================
 */
TM_SAFE
void*
list_iter_next (list_iter_t* itPtr)
{
    *itPtr = (*itPtr)->nextPtr;
    return (*itPtr)->dataPtr;
}


/* =============================================================================
 * allocNode
 * -- Returns NULL on failure
 * =============================================================================
 */
TM_SAFE
list_node_t*
allocNode (void* dataPtr)
{
    list_node_t* nodePtr = (list_node_t*)SEQ_MALLOC(sizeof(list_node_t));
    if (nodePtr == NULL) {
        return NULL;
    }

    TM_SHARED_WRITE(nodePtr->dataPtr, dataPtr);
    TM_SHARED_WRITE(nodePtr->nextPtr, (list_node*)NULL);

    //Was:
    //nodePtr->dataPtr = dataPtr;
    //nodePtr->nextPtr = NULL;

    return nodePtr;
}


/* =============================================================================
 * list_alloc
 * -- If NULL passed for 'compare' function, will compare data pointer addresses
 * -- Returns NULL on failure
 * =============================================================================
 */
TM_SAFE
list_t*
list_alloc (TM_SAFE long (*compare)(const void*, const void*))
{
    list_t* listPtr = (list_t*)SEQ_MALLOC(sizeof(list_t));
    if (listPtr == NULL) {
        return NULL;
    }

    TM_SHARED_WRITE(listPtr->head.dataPtr, (void*)NULL);
    TM_SHARED_WRITE(listPtr->head.nextPtr, (list_node*)NULL);
    TM_SHARED_WRITE(listPtr->size, 0L);

    //Was:
    //listPtr->head.dataPtr = NULL;
    //listPtr->head.nextPtr = NULL;
    //listPtr->size = 0;

    if (compare == NULL) {
        TM_SHARED_WRITE(listPtr->compare, &compareDataPtrAddresses);

        //Was:
        //listPtr->compare = &compareDataPtrAddresses; /* default */
    } else {
        TM_SHARED_WRITE(listPtr->compare, compare);

        //Was:
        //listPtr->compare = compare;
    }

    return listPtr;
}


/* =============================================================================
 * TMfreeNode
 * =============================================================================
 */
TM_SAFE
void
freeNode (list_node_t* nodePtr)
{
    SEQ_FREE(nodePtr);
}


/* =============================================================================
 * TMfreeList
 * =============================================================================
 */
TM_SAFE
void
freeList (list_node_t* nodePtr)
{
    if (nodePtr != NULL) {
        list_node_t* nextPtr = (list_node_t*)nodePtr->nextPtr;
        freeList(nextPtr);
        freeNode(nodePtr);
    }
}

/* =============================================================================
 * list_free
 * =============================================================================
 */
TM_SAFE
void
list_free (  list_t* listPtr)
{
    list_node_t* nextPtr = (list_node_t*)listPtr->head.nextPtr;
    freeList(nextPtr);
    SEQ_FREE(listPtr);
}


/* =============================================================================
 * list_isEmpty
 * -- Return TRUE if list is empty, else FALSE
 * =============================================================================
 */
TM_SAFE
bool_t
list_isEmpty (list_t* listPtr)
{
    return (listPtr->head.nextPtr == NULL);
}


/* =============================================================================
 * TMlist_getSize
 * -- Returns the size of the list
 * =============================================================================
 */
TM_SAFE
long
list_getSize (  list_t* listPtr)
{
    return (long)(listPtr->size);
}


/* =============================================================================
 * findPrevious
 * =============================================================================
 */
TM_SAFE
list_node_t*
findPrevious (list_t* listPtr, void* dataPtr)
{
    list_node_t* prevPtr = &(listPtr->head);
    list_node_t* nodePtr = prevPtr->nextPtr;
    unsigned long checker = 0;
    for (; nodePtr != NULL; nodePtr = nodePtr->nextPtr) {
        if (listPtr->compare(nodePtr->dataPtr, dataPtr) >= 0) {
            return prevPtr;
        }
        prevPtr = nodePtr;
	checker++;
	if(checker>10000000)
	  assert(0);//printf("checker in list fin);
    }

    return prevPtr;
}


/* =============================================================================
 * list_find
 * -- Returns NULL if not found, else returns pointer to data
 * =============================================================================
 */
TM_SAFE
void*
list_find (list_t* listPtr, void* dataPtr)
{
    list_node_t* nodePtr;
    list_node_t* prevPtr = findPrevious(listPtr, dataPtr);

    nodePtr = prevPtr->nextPtr;

    if ((nodePtr == NULL) ||
        (listPtr->compare(nodePtr->dataPtr, dataPtr) != 0)) {
        return NULL;
    }

    return (nodePtr->dataPtr);
}


TM_PURE
void dump_stack(){
  printf("stack trace 1: %lx %lx %lx %lx %lx \n", (uint64_t)__builtin_return_address(0), (uint64_t)__builtin_return_address(1), (uint64_t)__builtin_return_address(2), (uint64_t)__builtin_return_address(3), (uint64_t)__builtin_return_address(4));

}

/* =============================================================================
 * list_insert
 * -- Return TRUE on success, else FALSE
 * =============================================================================
 */
TM_SAFE
bool_t
list_insert (list_t* listPtr, void* dataPtr)
{
    list_node_t* prevPtr;
    list_node_t* nodePtr;
    list_node_t* currPtr;

    prevPtr = findPrevious(listPtr, dataPtr);
    currPtr = prevPtr->nextPtr; // shared_read

    // TM_SAFE compare()
#ifdef LIST_NO_DUPLICATES
    if ((currPtr != NULL) &&
        listPtr->compare(currPtr->dataPtr, dataPtr) == 0) {
      //dump_stack();
      //assert(0);
        return FALSE;
    }
#endif

    nodePtr = allocNode(dataPtr);
    if (nodePtr == NULL) {
      return FALSE;
    }

    // shared_writes
    TM_SHARED_WRITE(nodePtr->nextPtr, currPtr);
    TM_SHARED_WRITE(prevPtr->nextPtr, nodePtr);
    TM_SHARED_WRITE(listPtr->size, listPtr->size+1);

    //Was:
    //nodePtr->nextPtr = currPtr;
    //prevPtr->nextPtr = nodePtr;
    //listPtr->size++;

    return TRUE;
}


/* =============================================================================
 * list_remove
 * -- Returns TRUE if successful, else FALSE
 * =============================================================================
 */
TM_SAFE
bool_t
list_remove (list_t* listPtr, void* dataPtr)
{
    list_node_t* prevPtr;
    list_node_t* nodePtr;

    prevPtr = findPrevious(listPtr, dataPtr);

    nodePtr = prevPtr->nextPtr;
    if ((nodePtr != NULL) &&
        (listPtr->compare(nodePtr->dataPtr, dataPtr) == 0))
    {
        TM_SHARED_WRITE(prevPtr->nextPtr, nodePtr->nextPtr);
        TM_SHARED_WRITE(nodePtr->nextPtr, (list_node*)NULL);        
        freeNode(nodePtr);
        TM_SHARED_WRITE(listPtr->size, listPtr->size-1);

        //Was:
        //prevPtr->nextPtr = nodePtr->nextPtr;
        //nodePtr->nextPtr = NULL;
        //freeNode(nodePtr);
        //listPtr->size--;
        assert(listPtr->size >= 0);
        return TRUE;
    }

    return FALSE;
}


/* =============================================================================
 * list_clear
 * -- Removes all elements
 * =============================================================================
 */
TM_SAFE
void
list_clear (list_t* listPtr)
{
    freeList(listPtr->head.nextPtr);
    TM_SHARED_WRITE(listPtr->head.nextPtr, (list_node*)NULL);
    TM_SHARED_WRITE(listPtr->size, 0L);
    
    //Was:
    //listPtr->head.nextPtr = NULL;
    //listPtr->size = 0;
}


/* =============================================================================
 * TEST_LIST
 * =============================================================================
 */
#ifdef TEST_LIST


#include <assert.h>
#include <stdio.h>


static long
compare (const void* a, const void* b)
{
    return (*((const long*)a) - *((const long*)b));
}


static void
printList (list_t* listPtr)
{
    list_iter_t it;
    printf("[");
    list_iter_reset(&it, listPtr);
    while (list_iter_hasNext(&it, listPtr)) {
        printf("%li ", *((long*)(list_iter_next(&it, listPtr))));
    }
    puts("]");
}


static void
insertInt (list_t* listPtr, long* data)
{
    printf("Inserting: %li\n", *data);
    list_insert(listPtr, (void*)data);
    printList(listPtr);
}


static void
removeInt (list_t* listPtr, long* data)
{
    printf("Removing: %li\n", *data);
    list_remove(listPtr, (void*)data);
    printList(listPtr);
}


int
main ()
{
    list_t* listPtr;
#ifdef LIST_NO_DUPLICATES
    long data1[] = {3, 1, 4, 1, 5, -1};
#else
    long data1[] = {3, 1, 4, 5, -1};
#endif
    long data2[] = {3, 1, 4, 1, 5, -1};
    long i;

    puts("Starting...");

    puts("List sorted by values:");

    listPtr = list_alloc(&compare);

    for (i = 0; data1[i] >= 0; i++) {
        insertInt(listPtr, &data1[i]);
        assert(*((long*)list_find(listPtr, &data1[i])) == data1[i]);
    }

    for (i = 0; data1[i] >= 0; i++) {
        removeInt(listPtr, &data1[i]);
        assert(list_find(listPtr, &data1[i]) == NULL);
    }

    list_free(listPtr);

    puts("List sorted by addresses:");

    listPtr = list_alloc(NULL);

    for (i = 0; data2[i] >= 0; i++) {
        insertInt(listPtr, &data2[i]);
        assert(*((long*)list_find(listPtr, &data2[i])) == data2[i]);
    }

    for (i = 0; data2[i] >= 0; i++) {
        removeInt(listPtr, &data2[i]);
        assert(list_find(listPtr, &data2[i]) == NULL);
    }

    list_free(listPtr);

    puts("Done.");

    return 0;
}


#endif /* TEST_LIST */

#ifdef __cplusplus
}
#endif

/* =============================================================================
 *
 * End of list.c
 *
 * =============================================================================
 */

