#pragma once

#include <stdio.h>
#include <assert.h>

#include "timers.h"

//Handles printing of metrics and arguments

//stdout must be line buffered for the routines below to work
extern bool stdout_readied;
void test_ready_stdout();

//For printing runtime and compile time args
inline void test_compile_arg_print(
        unsigned long run_uid,
	const char* arg_name,
	const char* value
){
        assert(stdout_readied);
        printf(
                ">CompileArgs\t%lu\t%s\t%s\n",
                run_uid,
                arg_name,
                value
        );
}

inline void test_compile_arg_printi(
        unsigned long run_uid,
	const char* arg_name,
	unsigned long value
){
        assert(stdout_readied);
        printf(
                ">CompileArgs\t%lu\t%s\t%lu\n",
                run_uid,
                arg_name,
                value
        );
}

inline void test_args_print(
        unsigned long run_uid,
        const char* arg_name,
        unsigned long value
){
        assert(stdout_readied);
        printf(
                ">Args\t%lu\t%s\t%lu\n",
                run_uid,
                arg_name,
                value
        );
}

inline void test_args_prints(
        unsigned long run_uid,
        const char* arg_name,
        const char* value
){
        assert(stdout_readied);
        printf(
                ">Args\t%lu\t%s\t%s\n",
                run_uid,
                arg_name,
                value
        );
}

inline void test_metrics_print(
        unsigned long run_uid,
        unsigned long dump_uid,
        const char* metric_name,
        unsigned long value
){
        assert(stdout_readied);
        printf(
                ">Metrics\t%lu\t%lu\t%s\t%lu\n",
                run_uid,
                dump_uid,
                metric_name,
                value
        );
}

//Not for calculations, but just for human readability
inline void test_metrics_printd(
        unsigned long run_uid,
        unsigned long dump_uid,
        const char* metric_name,
        double value
){
        assert(stdout_readied);
        printf(
                ">Metrics\t%lu\t%lu\t%s\t%.3f\n",
                run_uid,
                dump_uid,
                metric_name,
                value
        );
}
