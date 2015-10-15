#pragma once

//We might get this from somewhere else
#ifndef HEADER_INLINE
#define HEADER_INLINE static inline __attribute__((always_inline))
#endif

static inline void cpu_relax(){
        asm volatile("pause" : : : "memory");
}
