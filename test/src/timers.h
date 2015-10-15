#pragma once

#include <time.h>

#include "common.h"

static inline unsigned long long rdtsc_begin(void)
{
  unsigned hi, lo;
  asm volatile (
      //"CPUID\n\t"
      "RDTSC\n\t"
      : "=a"(lo), "=d"(hi));
  return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}
static inline unsigned long long rdtsc_end(void)
{
  unsigned long long ret;

    unsigned hi, lo;
    asm volatile (
    "RDTSCP\n\t"
    "mov %%edx, %0\n\t"
    "mov %%eax, %1\n\t"
    //"CPUID\n\t"
    : "=r"(hi), "=r"(lo)
    :: "%rax", "%rbx", "%rcx", "%rdx", "memory");
    return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}

/*
 * Returns wallclock time in nanoseconds. May wrap around.
 */
static inline unsigned long wallclock(){
        struct timespec ts;
        int ret = clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

        const unsigned long TEN_9 = 1000000000;
        return ((unsigned long)ts.tv_sec) * TEN_9 + ts.tv_nsec;
}
