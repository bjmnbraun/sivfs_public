#include "gl.h"

_simple_allocator* gl_allocator = NULL;

gl_lock_t* gl_lock = NULL;

vector_simple_allocator<struct gl_state*>* gl_states = NULL;

thread_local struct gl_state* gl_state = NULL;

struct gl_stats* gl_stats = NULL;
