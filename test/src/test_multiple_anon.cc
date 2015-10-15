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

size_t FILE_SIZE = 4096;

//Same as test_mmap, but triggers page faults by truncating and resizing
//the file
void test_mmap_create(char* fname){
  size_t fsize = FILE_SIZE;
  int fhandle = open(fname, O_RDWR | O_TRUNC | O_CREAT, 0600);
  assert(fhandle >= 0);
  int rc = ftruncate(fhandle, fsize);
  assert(rc == 0);
  int mapflags = MAP_PRIVATE;
  char* ptr = (char*)mmap(NULL, fsize, PROT_READ | PROT_WRITE, mapflags, fhandle, 0);
  for(size_t i = 0; i < FILE_SIZE; i+= PGSIZE){
        ptr[i]++;
  }
  char* ptr2 = (char*)mmap(NULL, fsize, PROT_READ | PROT_WRITE, mapflags, fhandle, 0);
  //Check
  printf("Comparison: %p %p %d %d\n", ptr, ptr2, ptr[0], ptr2[0]);
  if (ptr[0] != ptr2[0]){
        printf("NOT THE SAME!\n");
  }

  //Close on exit
  //close(fhandle);
}

int main(int argc, char** argv){
    assert(argc >= 3);
    //In megabytes
    FILE_SIZE=atol(argv[2])*(1<<20);
    test_mmap_create(argv[1]);
    printf("Success!\n");
}
