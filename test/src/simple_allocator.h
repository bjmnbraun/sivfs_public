#pragma once

#include <cstddef>
#include <cassert>

#include "config.h"

//A simple allocator that divvies out virtual memory
//Not MT safe, must be used with locks held
//Free is a no-op
class _simple_allocator{
  public:
        _simple_allocator(void* mem, size_t size)
        : mem{mem}
        , size{size}
        {
        }

        void deallocate(void* var, size_t n){
                //assert(0);
#if DEBUG_SIMPLE_ALLOCATOR
                printf("simple_allocator ignoring deallocate\n");
#endif
        }

        void* allocate(size_t toAlloc){
                round_up_cache_line();
                
                if (size < toAlloc){
                        //OOM
#if DEBUG_SIMPLE_ALLOCATOR
                        printf(
                                "simple_allocator OOM %zd bytes at %p\n", 
                                toAlloc,
                                mem
                        );
#endif
                        return NULL;
                }
                void* toRet = mem;

#if DEBUG_SIMPLE_ALLOCATOR
                printf(
                        "simple_allocator allocated %zd bytes at %p\n", 
                        toAlloc,
                        mem
                );
#endif

                mem = (char*)mem + toAlloc;
                size -= toAlloc;
                return toRet;
        }

  private:
        void round_up_cache_line(){
                uintptr_t in_cache_line = ((uintptr_t)mem) & (64 - 1);
                if (in_cache_line){
                        uintptr_t toAdd = 64 - in_cache_line;
                        if (size < toAdd){
                                //Just empty us out
                                toAdd = size;
                        }

                        mem = (char*)mem + toAdd;
                        size -= toAdd;
                }
        }

        void* mem;
        size_t size;
};

//Adaptor for above to fit allocator concept
template <class T>
class simple_allocator {
  public:
        typedef T value_type;
        simple_allocator(_simple_allocator* allocator)
        : allocator{allocator}
        {
        }

        template <class U> simple_allocator(const simple_allocator<U>& other)
        : allocator{other.allocator}
        {
        }

        T* allocate(std::size_t n = 1){
                return (T*)allocator->allocate(n*sizeof(T));
        }

        void deallocate(T* p, std::size_t n){
                allocator->deallocate(p, n*sizeof(T));
        }

  private:
        _simple_allocator* allocator;
};

//Some typedefs for convenience
template <class T> 
using vector_simple_allocator = std::vector<T, simple_allocator<T> >;

//Some helper method for convenience
template <class T>
HEADER_INLINE void simple_allocator_allocate(/* out */ T*& out, _simple_allocator* allocator){
        simple_allocator<T> allocator_adaptor(allocator);
        out = allocator_adaptor.allocate();
        if (!out){
                throw std::runtime_error("Out of memory");
        }
}

