#include "locktable.h"

thread_local struct locktable_state* locktable_state = NULL;

locktable_shared_state* locktable_sstate = NULL;

_simple_allocator* locktable_allocator = NULL;
