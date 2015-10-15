#ifndef SIVFS_BACKGROUND_THREADS_H
#define SIVFS_BACKGROUND_THREADS_H

struct sivfs_shared_state;

int sivfs_init_background_work(struct sivfs_shared_state* sstate);

void sivfs_destroy_background_work(struct sivfs_shared_state* sstate);

#endif
