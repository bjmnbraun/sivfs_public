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
#include <string.h>
#include <stdint.h>

#include "sivfs.h"
#include "timers.h"

#define PGSIZE 4096ULL

#define MODE_DEFAULT 0
#define MODE_STRESS 1
#define MODE_OPEN 2
#define MODE_MMAP 3
#define MODE_NOCONCURRENT 4

bool TEST_CHECKPOINTING = false;

void sivfs_commit_update(struct sivfs_args_t* args){
        int sivfs_mod_fd = open("/dev/sivfs", O_RDWR);
        assert(sivfs_mod_fd != -1);

        int rc = ioctl(sivfs_mod_fd, SIVFS_TXN_COMMIT_UPDATE, args);
        if (rc){
                printf("RC was %d!\n", rc);
                assert(false);
        }

        assert(!args->aborted);

        close(sivfs_mod_fd);

        if (TEST_CHECKPOINTING){
                //Sleep after every time so that we can let the checkpointing thread
                //run. This increases the chance we read from a checkpoint.
                sched_yield();
                usleep(200000);
                sched_yield();
        }
}

void sivfs_print_page_info(void* address){
        int sivfs_mod_fd = open("/dev/sivfs", O_RDWR);
        assert(sivfs_mod_fd != -1);

        size_t buf_size = 256;
        char buf[buf_size] = {'a'};

        sivfs_args_t argstruct;
        sivfs_init_args(&argstruct);

        argstruct.target_address = address;
        argstruct.buf_size = buf_size;
        argstruct.buf = buf;

        int rc = ioctl(sivfs_mod_fd, SIVFS_GET_PAGE_INFO, &argstruct);
        assert(rc == 0);

        close(sivfs_mod_fd);

        printf("%s\n", buf);
}

void test_txn(void* address, uint64_t value){

        sivfs_args_t argstruct;
        sivfs_init_args(&argstruct);

        uint64_t prior_value = ((uint64_t*)address)[0];

        //Need to do the write and add the value to the writeset
        ((uint64_t*)address)[0] = value;
        DEFINE_WRITESET_ENTRY(entry, address, value);

        sivfs_add_to_writeset(
                &argstruct.ws,
                &entry,
                sizeof(entry)
        );

        sivfs_commit_update(&argstruct);

        //Clean up
        sivfs_destroy_args(&argstruct);
}

//x1 += add1, x2 += add2, transactionally.
//
//Should work even if x1 and x2 are the same.
void test_txn_add2(void* _x1, int64_t add1, void* _x2, int64_t add2){
        sivfs_args_t argstruct;
        sivfs_init_args(&argstruct);

        uint64_t* x1 = (uint64_t*)_x1;
        uint64_t* x2 = (uint64_t*)_x2;

        //Need to do the write and add the value to the writeset
        uint64_t nx1 = (*x1 += add1);
        DEFINE_WRITESET_ENTRY(entry, x1, nx1);
        sivfs_add_to_writeset(
                &argstruct.ws,
                &entry,
                sizeof(entry)
        );

        uint64_t nx2 = (*x2 += add2);
        DEFINE_WRITESET_ENTRY(entry2, x2, nx2);
        sivfs_add_to_writeset(
                &argstruct.ws,
                &entry2,
                sizeof(entry2)
        );

        sivfs_commit_update(&argstruct);

        //Clean up
        sivfs_destroy_args(&argstruct);
}

//Update all mmaps
void test_update(){

        sivfs_args_t argstruct;
        sivfs_init_args(&argstruct);

        sivfs_commit_update(&argstruct);

        //Clean up
        sivfs_destroy_args(&argstruct);
}

void WAIT(volatile char* signal, char value){
        const double GHZ = 2.4;
        const unsigned long WAIT_TIMEOUT = (unsigned long) (1e9 * GHZ);

        unsigned long now = rdtsc_begin();
        while(*signal != value){
                if (rdtsc_begin() - now > WAIT_TIMEOUT){
                        printf("EXPERIMENT INVALID - TIMEOUT ON WAIT\n");
                        break;
                }
        }
}

void test_mmap_concurrent(char* fname){
        printf("Starting concurrent mmap test\n");
        printf("Correct output is all 1s at the end ...\n");

        volatile char* shared_space = (volatile char*)mmap(
                NULL,
                4096,
                PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_SHARED,
                -1,
                0
        );

        size_t map_size = PGSIZE * 8;
        int mapflags = MAP_SHARED;

        shared_space[0] = 0;

        pid_t child = fork();
        if (child == 0){
                //Stage 0,
        stage0:
                //Open the file, read from a few pages
                int fhandle = open(fname, O_RDWR);
                assert(fhandle >= 0);
                char* ptr = (char*)mmap(
                        (void*)0x10000000000,
                        0x10000000000,
                        //map_size,
                        PROT_READ | PROT_WRITE,
                        mapflags | MAP_FIXED,
                        fhandle,
                        0
                );
                assert(ptr != MAP_FAILED);

                int fhandle2 = open(fname, O_RDWR);
                assert(fhandle2 >= 0);
                char* ptr2 = (char*)mmap(
                        //NULL,
                        (void*)0x20000000000,
                        0x10000000000,
                        //map_size,
                        PROT_READ | PROT_WRITE,
                        mapflags | MAP_FIXED,
                        fhandle2,
                        0
                );
                assert(ptr != MAP_FAILED);

                printf("Currently no pages faulted in, i.e.:\n");
                sivfs_print_page_info(ptr + PGSIZE * 1);
                sivfs_print_page_info(ptr + PGSIZE * 2);

                printf(
                        "0-2: %d %d\n",
                        ptr[PGSIZE*1],
                        ptr[PGSIZE*2]
                );

                printf("After initial read:\n");
                sivfs_print_page_info(ptr + PGSIZE * 1);
                sivfs_print_page_info(ptr + PGSIZE * 2);

                shared_space[0]++;
                WAIT(shared_space, 2);
                //Stage 2
        stage2:
                test_update();

                printf("Faulted in (possibly workingset) page looks like:\n");
                sivfs_print_page_info(ptr + PGSIZE * 1);
                sivfs_print_page_info(ptr + PGSIZE * 2);

                //Verify that we saw the changes made in stage 1
                printf(
                        "2-1: %d %d %d\n",
                        ptr[PGSIZE*1],
                        ptr[PGSIZE*2],
                        ptr[PGSIZE*3]
                );

                printf("Faulted in (possibly workingset) page looks like:\n");
                sivfs_print_page_info(ptr + PGSIZE * 1);
                sivfs_print_page_info(ptr + PGSIZE * 2);

                //Verify again, but this time wait first to read from checkpoint
                sched_yield();
                usleep(500000);
                sched_yield();
                test_update();

                printf("Faulted in (possibly workingset) page looks like:\n");
                sivfs_print_page_info(ptr + PGSIZE * 1);
                sivfs_print_page_info(ptr + PGSIZE * 2);

                printf(
                        "2-2: %d %d %d %d\n",
                        ptr[PGSIZE*1],
                        ptr[PGSIZE*2],
                        ptr[PGSIZE*3],
                        ptr[PGSIZE*4]
                );

                printf("Faulted in (possibly workingset) page looks like:\n");
                sivfs_print_page_info(ptr + PGSIZE * 1);
                sivfs_print_page_info(ptr + PGSIZE * 2);
                printf("Faulted in (probably checkpoint) page looks like:\n");
                sivfs_print_page_info(ptr + PGSIZE * 4);

                //Do a write
                ptr[PGSIZE*4]++;
                printf("Faulted in (MUST BE workingset) page looks like:\n");
                sivfs_print_page_info(ptr + PGSIZE * 4);

                shared_space[0]++;

        child_out:
                munmap(ptr, map_size);
                close(fhandle);
                munmap(ptr2, map_size);
                close(fhandle2);

                //Fork bomb if we return! Exit!
                exit(0);
        }

        WAIT(shared_space, 1);
        //Stage 1
stage1:
        //Open the file, commit a transaction
        int fhandle = open(fname, O_RDWR);
        assert(fhandle >= 0);
        char* ptr = (char*)mmap(
                //NULL,
                (void*)0x30000000000,
                0x10000000000,
                //map_size,
                PROT_READ | PROT_WRITE,
                mapflags | MAP_FIXED,
                fhandle,
                0
        );
        assert(ptr != MAP_FAILED);

        int fhandle2 = open(fname, O_RDWR);
        assert(fhandle2 >= 0);
        char* ptr2 = (char*)mmap(
                //NULL,
                (void*)0x40000000000,
                0x10000000000,
                //map_size,
                PROT_READ | PROT_WRITE,
                mapflags | MAP_FIXED,
                fhandle2,
                0
        );
        assert(ptr != MAP_FAILED);

        test_txn(ptr + PGSIZE * 3, 7);
        test_txn(ptr + PGSIZE * 1, 1);
        //Add -3 from two places to get *3 down to 1
        test_txn_add2(ptr + PGSIZE * 3, -3, ptr2 + PGSIZE * 3, -3);
        test_txn(ptr + PGSIZE * 4, 1);
        printf(
                "1: %d %d %d %d\n",
                ptr[PGSIZE*1],
                ptr[PGSIZE*2],
                ptr[PGSIZE*3],
                ptr[PGSIZE*4]
        );

        //Do a non-transactional write, to show that isolation is happening
        ptr[PGSIZE * 3] = 42;

        shared_space[0]++;

        munmap(ptr, map_size);
        close(fhandle);
        munmap(ptr2, map_size);
        close(fhandle2);

//Just wait until the test finishes.
stage3:
        WAIT(shared_space, 3);
}

void test_mmap(char* fname){
        int fhandle = open(fname, O_RDWR);
        assert(fhandle >= 0);
        struct stat filestat;
        int rc = fstat(fhandle, &filestat);
        assert(rc == 0);
        size_t fsize = filestat.st_size;
        int mapflags = MAP_SHARED;
        size_t map_size = PGSIZE * 4;
        char* ptr = (char*)mmap(NULL, map_size, PROT_READ | PROT_WRITE, mapflags, fhandle, 0);
        assert(ptr != MAP_FAILED);
        //printf("%.*s\n", 30, ptr);
        //Unix files always have a terminating line return
        //printf("%.*s", 30, ptr);
        if (fsize){
                //printf("%.*s", 1024, ptr);
                //printf("%.*s\n", PGSIZE*3, ptr + 1000);
                printf(
                        "2 %d %d %d\n",
                        ptr[PGSIZE*1],
                        ptr[PGSIZE*2],
                        ptr[PGSIZE*3]
                );
        } else {
                printf("Empty mmap.\n");
        }
        //Close should happen whenever we unmmap and close file
        //(in either order)
        munmap(ptr, map_size);
        close(fhandle);
}

//Same as test_mmap, but triggers page faults by truncating and resizing
//the file
void test_mmap_create(char* fname, int mode){
        size_t fsize = PGSIZE*(1<<20ULL);
        int fhandle = open(fname, O_RDWR | O_TRUNC | O_CREAT, 0600);
        int rc = 0;
        char* ptr;
        int mapflags = MAP_SHARED;

        if (mode == MODE_OPEN){
                goto close;
        }

        assert(fhandle >= 0);
        printf("Calling truncate...\n");
        rc = ftruncate(fhandle, fsize);
        assert(rc == 0);
        printf("Calling mmap...\n");
        ptr = (char*)mmap(
                NULL,
                fsize,
                PROT_READ | PROT_WRITE,
                mapflags,
                fhandle,
                0
        );
        assert(ptr != MAP_FAILED);
        if (mode == MODE_MMAP){
                goto munmap;
        }
        printf(
                "Create-1: %d %d %d\n",
                ptr[PGSIZE*1],
                ptr[PGSIZE*2],
                ptr[PGSIZE*3]
        );
        //ptr[PGSIZE*2]++;

        //Try a write txn
        test_txn(ptr + PGSIZE * 2, 1);

        printf(
                "Create-2: %d %d %d %d\n",
                ptr[PGSIZE*1],
                ptr[PGSIZE*2],
                ptr[PGSIZE*3],
                ptr[PGSIZE*4]
        );


        if(mode == MODE_STRESS){
                for(size_t i = 0; i < fsize; i+=PGSIZE){
                        ptr[i]++;
                }
        }

munmap:
        munmap(ptr, fsize);
close:
        close(fhandle);
}

int main(int argc, char** argv){
        assert(argc >= 3);
        int mode = atoi(argv[2]);
        test_mmap_create(argv[1], mode);
        if (mode == MODE_DEFAULT){
                TEST_CHECKPOINTING = false;
                test_mmap_concurrent(argv[1]);
                //TEST_CHECKPOINTING = true;
                //test_mmap_concurrent(argv[1]);
        }
        if (mode == MODE_NOCONCURRENT){
                test_mmap(argv[1]);
        }
        printf("Main exited successfully!\n");
}
