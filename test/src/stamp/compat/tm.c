#include "tm.h"

int stamp_initializing = 0;

//thread locals are not reset on fork, so can't do it this way!
//thread_local bool thread_entered = false;
