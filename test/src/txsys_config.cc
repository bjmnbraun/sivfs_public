#include <stdio.h>

#include "config.h"
#include "metrics.h"
#include "txsys_config.h"

//For printing TX_SYS compile parameters

//Undo default TX_SYS_VALUE
#undef TX_SYS_VALUE

#define F(X) #X,
static const char* MY_ARG_NAMES[] = {
	TX_SYS_ARGS
};
#undef F 

#define F(X) X
#define TX_SYS_VALUE(X) #X,
static const char* MY_ARG_VALUES[] = {
	TX_SYS_ARGS
};
#undef TX_SYS_VALUE
#undef F 

void txsys_compile_args_print(unsigned long run_uid){
        const char** argName = MY_ARG_NAMES;
        const char** argValue = MY_ARG_VALUES;
#define F(X) \
	test_compile_arg_print(run_uid, *argName, *argValue); \
        argName++;\
        argValue++;

	TX_SYS_ARGS
#undef F
}
