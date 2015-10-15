#include "fine_grained_locking.h"

_simple_allocator* fgl_allocator = NULL;

simple_mutex* fgl_lock = NULL;

vector_simple_allocator<struct fine_grained_locking_state*>* fgl_states = NULL;

thread_local struct fine_grained_locking_state* fgl_state = NULL;

struct fine_grained_locking_stats* fgl_stats = NULL;
