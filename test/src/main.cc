#include <sys/ioctl.h>
#include <sys/stat.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cstdio>
#include <sys/mman.h>
#include <linux/sched.h>
#include <sched.h>
#include <unistd.h>
#include <pthread.h>

#define PGSIZE 4096

void test_mmap(char* fname){
  int fhandle = open(fname, O_RDWR);
  assert(fhandle >= 0);
  struct stat filestat;
  int rc = fstat(fhandle, &filestat);
  assert(rc == 0);
  size_t fsize = filestat.st_size;
  //int mapflags = MAP_PRIVATE;
  int mapflags = MAP_SHARED;
  char* ptr = (char*)mmap(NULL, PGSIZE*3, PROT_READ | PROT_WRITE, mapflags, fhandle, 0);
  assert(ptr != MAP_FAILED);
  //printf("%.*s\n", 30, ptr);
  //Unix files always have a terminating line return
  //printf("%.*s", 30, ptr);
  if (fsize){
    //printf("%.*s", 1024, ptr);
    //printf("%.*s\n", PGSIZE*3, ptr + 1000);
    printf("%d\n", ptr[PGSIZE*2]);
  } else {
    printf("Empty mmap.\n");
  }
  close(fhandle);
}

//Same as test_mmap, but triggers page faults by truncating and resizing
//the file
void test_mmap_create(char* fname){
  size_t fsize = PGSIZE*100;
  int fhandle = open(fname, O_RDWR | O_TRUNC | O_CREAT, 0600);
  assert(fhandle >= 0);
  int rc = ftruncate(fhandle, fsize);
  assert(rc == 0);
  int mapflags = MAP_SHARED;
  char* ptr = (char*)mmap(NULL, fsize, PROT_READ | PROT_WRITE, mapflags, fhandle, 0);
  assert(ptr != MAP_FAILED);
  printf("%d %d %d\n", ptr[PGSIZE*1], ptr[PGSIZE*2], ptr[PGSIZE*3]);
  ptr[PGSIZE*4]++;
  printf("%d %d %d %d\n", ptr[PGSIZE*1], ptr[PGSIZE*2], ptr[PGSIZE*3], ptr[PGSIZE*4]);
  close(fhandle);
}

int main(int argc, char** argv){
    assert(argc >= 2);
    test_mmap_create(argv[1]);
    test_mmap(argv[1]);

#if 0
    assert(argc >= 2);
    int protect_process = atoi(argv[1]);

    printf("%d\n", protect_process);

    if (0){
        struct sched_param fifo_param;
        fifo_param.sched_priority = 99; 
        pthread_setschedparam(pthread_self(), 
                SCHED_FIFO, &fifo_param);
    }

    if (protect_process){
        int fd = open("/dev/timing_defense", O_RDWR);
        assert(fd != -1);
        struct timing_defense_stats* stats = NULL;
        for(int i = 0; i < 2; i++){
         stats = 
            (struct timing_defense_stats*) mmap(
            NULL,
            sizeof(struct timing_defense_stats),
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            0
            );
        assert(stats);

        timing_defense_args_t argstruct;
        int ret = ioctl(fd, TIMING_DEFENSE_ENABLE, &argstruct);
        assert(!ret);

/*
        for(int i = 0; i < 1000000000; i++){
            asm volatile("nop" : : : "memory");
        }
        */
        for(int i = 0; i < 200; i++){
            sched_yield();
        }

        printf("%Lu %Lu %Lu %Lu\n", stats->nvcsw, stats->nivcsw,
        stats->ninterrupts, stats->nticks);
        }
    }

    while(1){
    }
#endif
}
