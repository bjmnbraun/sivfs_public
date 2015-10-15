#include <sys/ioctl.h>
#include <sys/stat.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cstdio>
#include <sys/mman.h>
#include <sys/wait.h>
#include <linux/sched.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <atomic>
#include <thread>

#include "sivfs.h"
#include "txsys.h"
#include "timers.h"
#include "metrics.h"
#include "test_random.h"

//Small library of functions unit tests can use:
#define _str(s) #s
#define str(s) _str(s)

const char* test_folder = "/mnt/sivfs/bin/src/unit_tests/" str(TEST_NAME);

struct test_file {
        void* address;
        int fhandle;
        size_t fsize;
};

//Multiples of 2^40 = (12+12+12+9)*2. Designed to give good alignment with
//puds on x86_64.
#define MAP_ADDR_0 ((void*)0x30000000000)
#define MAP_ADDR_1 ((void*)0x40000000000)
#define MAP_ADDR_2 ((void*)0x50000000000)

struct test_file open_test_file(
        const char* _filename,
        bool create,
        void* address
){
        std::string filename =
                std::string(test_folder) + std::string("/") + std::string(_filename)
        ;
        int fhandle = open(
                filename.c_str(),
                O_RDWR | (create? (O_TRUNC | O_CREAT) : 0),
                0600
        );
        assert(fhandle >= 0);
        int mapflags = MAP_SHARED | (address ? MAP_FIXED : 0);
        //TODO right now we create very large files, but really this makes
        //debugging harder because fixed-size files should segfault if we
        //address past them.
        //
        //2^40 = (12+12+12+9)*2
        size_t fsize = 0x10000000000;
        int rc = 0;
        if (create){
                rc = ftruncate(fhandle, fsize);
                assert(rc == 0);
        }
        void* ptr = (char*)mmap(
                address,
                fsize,
                PROT_READ | PROT_WRITE,
                mapflags,
                fhandle,
                0
        );
        assert(ptr != MAP_FAILED);

        //Set up allocator. Ideally, we don't move around an mmap after calling
        //this, but if we must we can by calling add_mmap again (which will
        //invalidate some cache entries in the allocator.)
        rc = sivfs_allocator_add_mmap(address, 0, fsize);
        assert(rc == 0);

        return {
                .address = ptr,
                .fhandle =fhandle,
                .fsize = fsize
        };
}

thread_local unsigned threadID = -1;
size_t _threadCount = 1;
size_t threadCount(size_t n = -1){
        if (n != (size_t)-1){
                _threadCount = n;
        }
        return _threadCount;
}

//zero timeout means never timeout
size_t _timeout = (size_t)0;
size_t timeout(size_t n = -1){
        if (n != (size_t)-1){
                _timeout = n;
        }
        return _timeout;
}
int _argc = -1;
const char** _argv = NULL;
void test_args(int* argc, const char*** argv){
        *argc = _argc;
        *argv = _argv;
}
//Done with library

#include str(TEST_FILE)

volatile char* shared_space;
const size_t shared_space_size = 128000;

unsigned long RUN_UID = -1;

#define APP_METRIC(x) F(x)

#define APP_METRICS\
        APP_METRIC(nNanoseconds) \

class AppMetrics {
  public:
#define F(x) uint64_t x;
        APP_METRICS
#undef F
};
AppMetrics* metrics = NULL;

tx_sys_stats_t* tx_sys_stats = NULL;

void setup_all(){
        //Make a metrics tag that is "unique"
        RUN_UID = wallclock();

        shared_space = (volatile char*)mmap(
                NULL,
                shared_space_size,
                PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_SHARED,
                -1,
                0
        );
        assert(shared_space);

        metrics = (AppMetrics*)(shared_space + 32);
        new (metrics) AppMetrics();

        tx_sys_stats = new tx_sys_stats_t({});
}

#if TX_SYS == TX_SYS_SIVFS
void setup_sivfs(){
        sivfs_init();
}
#elif TX_SYS == TX_SYS_GL
void setup_gl(){
        int mapflags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                db_address,
                db_size,
                PROT_READ | PROT_WRITE,
                mapflags,
		-1,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == db_address);

        db_records = (record_t*)ptr;

        gl_init((char*)shared_space + 4096, shared_space_size - 4096);
}
#elif TX_SYS == TX_SYS_LOCKTABLE
void setup_locktable(){
        int mapflags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                db_address,
                db_size,
                PROT_READ | PROT_WRITE,
                mapflags,
		-1,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == db_address);

        db_records = (record_t*)ptr;

        locktable_init((char*)shared_space + 4096, shared_space_size - 4096);
}
#elif TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
void setup_fine_grained_locking(){
        int mapflags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                db_address,
                db_size,
                PROT_READ | PROT_WRITE,
                mapflags,
		-1,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == db_address);

        db_records = (record_t*)ptr;

        fgl_init((char*)shared_space + 4096, shared_space_size - 4096);
}
#else
        //Do nothing. We will assert in main.
#endif

void WAIT(volatile char* signal, char value){
        while(*signal != value){
                cpu_relax();
        }
}

void _run(size_t id){
        WAIT(shared_space + 0, 1);

        threadID = id;
        run();

        printf("Thread %d exiting!\n", threadID);
}

tx_sys_stats_t last_dump_tx_sys_stats{};
AppMetrics last_dump_AppMetrics{};

void dump_metrics(unsigned long DUMP_UID){
        //TODO dump compile-time and runtime args too?
	tx_sys_get_stats(tx_sys_stats);

        metrics->nNanoseconds = wallclock();

        if (DUMP_UID == (unsigned long)-1){
                test_ready_stdout();
		//First dump. Dump args and initialize metrics.
		txsys_compile_args_print(RUN_UID);

                const char* binary_name = basename(_argv[0]);
#define F(x) test_args_print( \
        RUN_UID, \
        "AppArgs." #x, \
        x \
);

        F(_threadCount)

#undef F

#define F(x) test_args_prints( \
        RUN_UID, \
        "AppArgs." #x, \
        x \
);

        F(binary_name)

#undef F
                goto out;
        }

#define F(x) test_metrics_print( \
        RUN_UID, \
        DUMP_UID, \
        "TXSysMetrics." #x, \
        tx_sys_stats->x - last_dump_tx_sys_stats.x \
);

        TX_SYS_STATS

#undef F

#define F(x) test_metrics_print( \
        RUN_UID, \
        DUMP_UID, \
        "AppMetrics." #x, \
        metrics->x - last_dump_AppMetrics.x \
);

        APP_METRICS

#undef F

out:
        //Save snapshot
        last_dump_tx_sys_stats = *tx_sys_stats;
        last_dump_AppMetrics = *metrics;
}

void sigchld_hdl_error(int sig){
        assert(0);
}

int main(int argc, const char** argv){

        _argc = argc;
        _argv = argv;

        unsigned long start_time;
        int threads_running;

        setup_all();

        struct sigaction act;

        std::string child_pids;

        sigset_t set_SIGCHLD;
        sigemptyset(&set_SIGCHLD);
        sigaddset(&set_SIGCHLD, SIGCHLD);
        if (sigprocmask(SIG_BLOCK, &set_SIGCHLD, NULL) < 0){
                fprintf(stderr, "sigprocmask error\n");
                goto error0;
        }

        //There is some debate on whether the following is necessary. Since we
        //are BLOCK'ing SIGCHLD, as above, and manually picking it up with
        //sigtimedwait, this handler should _never be called_ but set it
        //anyway due to various stuffy interpretations of posix.
        act = {0};
        act.sa_handler = sigchld_hdl_error;
        if (sigaction(SIGCHLD, &act, 0)){
                fprintf(stderr, "sigaction error\n");
                goto error0;
        }


#if TX_SYS == TX_SYS_SIVFS
        setup_sivfs();
#elif TX_SYS == TX_SYS_GL
	setup_gl();
#elif TX_SYS == TX_SYS_LOCKTABLE
        setup_locktable();
#elif TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
        setup_fine_grained_locking();
#else
        assert(0);
#endif

        setup();

        threads_running = 0;

        for(size_t i = 0; i < _threadCount; i++){
#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
                //Lambda thread spawn syntax
                std::thread t1([i](){
                       _run(i);
                });
                t1.detach();
#else
                int child = fork();
                if (child == 0){
                        _run(i);
                        //Exit very important here
                        exit(0);
                }
                if (child == -1){
                        fprintf(stderr, "fork error");
                        goto error0;
                }
#endif
                threads_running++;

                child_pids += std::to_string(child);
                child_pids += " ";
        }

        dump_metrics(-1);

        printf(
                "To debug break main:wake_up_workers then attach gdb instances to pids: %s\n",
                child_pids.c_str()
        );

wake_up_workers:
        //Wake up the workers
        shared_space[0] = 1;

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
                //??? how to do this with pthreads?
#else
        start_time = wallclock();
        while(threads_running){
                unsigned long t_now = wallclock();

                //Wait for at most a second for SIGCHLDs. There is no
                //possibility of missing a wakeup since we have blocked the
                //signal above.
                while(threads_running){
                        unsigned long t_now_now = wallclock();
                        unsigned long t_waiting = t_now_now - t_now;
                        unsigned long NS_PER_SECOND = 1000UL * 1000UL * 1000UL;
                        if (t_waiting >= NS_PER_SECOND - 1){
                                //We waited for more than a second.
                                break;
                        }

                        siginfo_t siginfo;
                        struct timespec remaining_timeout = {
                                0,
                                (long int)((NS_PER_SECOND-1)-t_waiting)
                        };
                        int signal = sigtimedwait(
                                &set_SIGCHLD,
                                &siginfo,
                                &remaining_timeout
                        );
                        int sigtimedwait_errno = errno;

                        if (signal == -1){
                                if (sigtimedwait_errno == EINTR || sigtimedwait_errno == EAGAIN){
                                        //Neither of these are true errors. Just keep
                                        //trucking.
                                        continue;
                                } else {
                                        fprintf(stderr, "sigtimedwait error%d\n", signal);
                                        goto error0;
                                }
                        } else {
                                //We are only listening for SIGCHLD...
                                assert(signal == SIGCHLD);
                        }

                        //sigtimedwait doesn't actually remove the child death
                        //event from the wait queue, so we can still do this:
                        while(threads_running){
                                int status;
                                int wpid = waitpid(-1, &status, WNOHANG);
                                if (wpid < 0){
                                        fprintf(stderr, "waitpid error %d\n", wpid);
                                        goto error0;
                                }
                                if (wpid > 0){
                                        if (status){
                                                fprintf(
                                                        stderr,
                                                        "child exited with error code %d\n",
                                                        status
                                                );
                                                goto error0;
                                        }

                                        threads_running--;
                                        continue;
                                }
                                //Wait queue is empty. Go back to waiting for signal,
                                //if threads_running is still true.
                                break;
                        }
                }

                //Dump metrics
                unsigned long DUMP_UID = wallclock();
                dump_metrics(DUMP_UID);

                //Check timeout
                t_now = wallclock();
                unsigned long time_running =
                        (unsigned long)((t_now - start_time)/1e9)
                ;
                if (timeout() && time_running >= timeout()){
                        //Timeout!
                        printf("Timeout. This may not be an error.\n");
                        kill(0, SIGTERM);
                        goto out;
                }
        }
#endif


#ifdef TEST_CHECK
        check();
#endif

        //Hmm. We can actually exit here.
        //shared_space[1] = 1;

out:
        return 0;

error0:
        kill(0, SIGTERM);
        //Shouldn't reach this line, but whatever
        return 1;
}

