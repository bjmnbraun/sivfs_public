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
#include <string.h>
#include <atomic>
#include <thread>

#include "sivfs.h"
#include "txsys.h"
#include "timers.h"
#include "metrics.h"
#include "test_random.h"

volatile char* shared_space;
const size_t shared_space_size = 128000;
int db_fhandle;

const char* db_fname = "/mnt/sivfs/ycsb_db";
void* const db_address = (void*)(0xE00ULL<<32ULL);

struct record_t {
        volatile uint64_t values[12];
#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
        fine_grained_lock_t lock;
#endif
};

const size_t RECORD_SIZE = sizeof(record_t);
//const size_t READ_PERCENTAGE = 95;
const size_t WRITE_THREAD_PERCENTAGE = 50;
//const size_t READ_PERCENTAGE = 100;
const size_t db_nRecords = 50*1000*100;
//const size_t db_nRecords = 50*1000;
//const size_t db_nRecords = 50;
const size_t db_size = 0x100ULL << 32ULL; //db_nRecords * RECORD_SIZE;

//Curretly uniform
const uint64_t ZIPF_PARAMETER = 0;

size_t nThreads = 4;

record_t* db_records = NULL;

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

        //Create the database. TODO prefill or load from file?
        printf("About to allocate %f GB\n", db_size / (double)(1ULL << 30));

        int fhandle = open(db_fname, O_RDWR | O_TRUNC | O_CREAT, 0600);
        assert(fhandle >= 0);
        int rc = ftruncate(fhandle, db_size);
        assert(rc == 0);
        close(fhandle);

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

void thread_enter_sivfs(){
        int fhandle = open(db_fname, O_RDWR);
        assert(fhandle >= 0);
        //??? Is this necessary?
        int mapflags = MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                db_address,
                db_size,
                PROT_READ | PROT_WRITE,
                mapflags,
                fhandle,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == db_address);

        db_records = (record_t*)ptr;
}

void thread_enter_gl(){
}

void thread_enter_locktable(){
}

void WAIT(volatile char* signal, char value){
        while(*signal != value){
                cpu_relax();
        }
}

void run(size_t id){
        WAIT(shared_space + 0, 1);

        sleep(1);

        bool aborted;
        TM_COMMIT_UPDATE(aborted);

        while(true){
                const size_t nOpsPerTxn = 4;

                size_t coins [nOpsPerTxn * 2];
                test_random(coins, sizeof(coins));

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
                //TODO need to gather the records, sort by address,
                //then acquire.
                for(size_t j = 0; j < nOpsPerTxn; j++){
                        size_t i = coins[j*2]%db_nRecords;
                        struct record_t& record = db_records[i];
                        FGL_ADD_LOCK(&record.lock);
                }
                FGL_LOCK_ALL();
#else
                retry:
                try {
#endif
                for(size_t j = 0; j < nOpsPerTxn; j++){
                        size_t i = coins[j*2]%db_nRecords;

                        size_t readCheck = coins[j*2+1];
                        bool shouldWrite = id < nThreads * WRITE_THREAD_PERCENTAGE / 100;//readCheck % 100 >= READ_PERCENTAGE;

                        struct record_t& record = db_records[i];
                        for(size_t k = 0; k < 12; k++){
                                uint64_t nValue =
                                        TM_READ(record.values + k)
                                        | (1ULL << id)
                                ;

                                if (shouldWrite){
                                        TM_WRITE(
                                                record.values + k,
                                                nValue
                                        );
                                }
                        }
                }

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
                TM_COMMIT_UPDATE(aborted);
#else

                } catch (const TM_ABORTED_EXCEPTION& e){
                }
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        goto retry;
                }
#endif

                //Break condition
                if (shared_space[1] == 1){
                        break;
                }

                /*
                if (__builtin_popcount(nValue) >= 2){
                        printf("collision achieved! %lu %lu\n", nValue);
                        break;
                }
                */
        }

        printf("Exiting!\n");
}

tx_sys_stats_t last_dump_tx_sys_stats{};
AppMetrics last_dump_AppMetrics{};

int _argc;
const char** _argv;

void dump_metrics(unsigned long DUMP_UID){
        //TODO dump compile-time and runtime args too?

	tx_sys_get_stats(tx_sys_stats);

        metrics->nNanoseconds = wallclock();

        if (DUMP_UID == (unsigned long)-1){
                test_ready_stdout();
		//First dump. Dump args and initialize metrics. 
		txsys_compile_args_print(RUN_UID);

#define F(x) test_args_print( \
        RUN_UID, \
        "AppArgs." #x, \
        x \
);

        F(nThreads)
        F(ZIPF_PARAMETER)
        //F(READ_PERCENTAGE)
        F(WRITE_THREAD_PERCENTAGE)
        F(RECORD_SIZE)
        F(db_nRecords)

#undef F
                const char* binary_name = basename(_argv[0]);

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


int main(int argc, const char** argv){

        if (argc >= 2){
                nThreads = atoi(argv[1]);
        }
        _argc = argc;
        _argv = argv;

        setup_all();

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

        for(size_t i = 0; i < nThreads; i++){
#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
                //Lambda thread spawn syntax
                std::thread t1([i](){
                       run(i);
                });
                t1.detach();
#else
                int child = fork();
                if (child == 0){
#if TX_SYS == TX_SYS_SIVFS
			thread_enter_sivfs();
#elif TX_SYS == TX_SYS_GL
			thread_enter_gl();
#elif TX_SYS == TX_SYS_LOCKTABLE
                        thread_enter_locktable();
#else
                        assert(0);
#endif
                        run(i);
                        //Exit very important here
                        exit(0);
                }
#endif
        }

        dump_metrics(-1);

        //Wake up the workers
        shared_space[0] = 1;

        for(size_t i = 0; i < 10; i++){
                sleep(1);

                unsigned long DUMP_UID = wallclock();
                dump_metrics(DUMP_UID);
        }

        //Hmm. We can actually exit here.
        shared_space[1] = 1;
}

