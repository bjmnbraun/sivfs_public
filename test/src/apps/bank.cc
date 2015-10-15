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

#include "sivfs.h"
#include "timers.h"
#include "metrics.h"
#include "test_random.h"

//Simple bank benchmark, not a realistic performance workload
//but good for teasing out concurrency errors such as nonatomic updates

int sivfs_mod_fd = -1;
struct sivfs_args_t sivfs_args;
volatile char* shared_space;
int db_fhandle;
const char* db_fname = "/mnt/sivfs/bank_db";

void* const db_address = (void*)(0xE00ULL<<32ULL);

struct record_t {
        volatile uint64_t value;
};

const size_t RECORD_SIZE = sizeof(record_t);
const size_t READ_PERCENTAGE = 95;
const size_t db_nRecords = 50*1000;
const size_t db_size = db_nRecords * RECORD_SIZE;

record_t* const db_records = (record_t*)db_address;

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

void sivfs_commit_update(/*out*/ bool& aborted){
        int rc = ioctl(sivfs_mod_fd, SIVFS_TXN_COMMIT_UPDATE, &sivfs_args);
        assert(rc == 0);
        aborted = sivfs_args.aborted;
}

void sivfs_write(volatile void* address, uint64_t value){
        ((uint64_t*)address)[0] = value;

        DEFINE_WRITESET_ENTRY(entry, address, value);

        sivfs_add_to_writeset(
                &sivfs_args.ws,
                entry
        );
}

void setup(){
        //Make a metrics tag that is "unique"
        RUN_UID = wallclock();

        sivfs_mod_fd = open("/dev/sivfs", O_RDWR);
        assert(sivfs_mod_fd != -1);

        shared_space = (volatile char*)mmap(
                NULL,
                4096,
                PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_SHARED,
                -1,
                0
        );
        assert(shared_space);

        metrics = (AppMetrics*)(shared_space + 32);
        new (metrics) AppMetrics();

        //Create the database. TODO prefill or load from file?

        printf("About to allocate %f GB\n", db_size / (double)(1ULL << 30));

        int fhandle = open(db_fname, O_RDWR | O_TRUNC | O_CREAT, 0600);
        assert(fhandle >= 0);
        int rc = ftruncate(fhandle, db_size);
        assert(rc == 0);

        //Fill it with 5's
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
        bool aborted;
        sivfs_commit_update(aborted);
        for(size_t i = 0; i < db_nRecords; ){
                for(size_t j = 0; j < 1000 && i < db_nRecords; j++, i++){
                        sivfs_write(
                                (void*)&db_records[i].value,
                                5
                        );
                }
                bool aborted;
                sivfs_commit_update(aborted);
        }
        munmap(db_address, db_size);
        close(fhandle);
        printf("Database created.");

        //Prepare args. Fork will copy a properly initialized version
        sivfs_init_args(&sivfs_args);
        //Zero initialization
        sivfs_args.stats = new struct sivfs_stats({});
}

void thread_enter(){
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
}

void WAIT(volatile char* signal, char value){
        while(*signal != value){
                cpu_relax();
        }
}

void run_analysis(size_t id){
        bool aborted;
        sivfs_commit_update(aborted);

        uint64_t sum = 0;
        for(size_t i = 0; i < db_nRecords; i++){
                sum += db_records[i].value;
        }

        uint64_t correct_sum = 5 * db_nRecords;
        if (sum != correct_sum){
                printf("BAD SUM: %zd should be %zd\n", sum, correct_sum);
        }

        sivfs_commit_update(aborted);
}

void run(size_t id){
        WAIT(shared_space + 0, 1);

        bool aborted;
        sivfs_commit_update(aborted);

        size_t iters_until_analyze = 0;
        while(true){
                if (++iters_until_analyze == 100000){
                        run_analysis(id);
                        iters_until_analyze = 0;
                }

                const size_t nOpsPerTxn = 1;

                size_t coins [nOpsPerTxn * 3];
                test_random(coins, sizeof(coins));

                size_t i = coins[0]%db_nRecords;
                size_t j = coins[1]%db_nRecords;
                size_t readCheck = coins[2];

                bool shouldWrite = readCheck % 100 >= READ_PERCENTAGE;

                //TM_LOAD
                uint64_t src = db_records[i].value;

                if (src > 0){
                        if (shouldWrite){
                                //TM_STORE
                                sivfs_write(
                                        (void*)&db_records[i].value,
                                        src-1
                                );
                                //TM_LOAD
                                //TM_STORE
                                sivfs_write(
                                        (void*)&db_records[j].value,
                                        db_records[j].value+1
                                );
                        }
                }

                sivfs_commit_update(aborted);

                //Break condition
                if (shared_space[1] == 1){
                        break;
                }

        }

        printf("Exiting!\n");
}

struct sivfs_stats last_dump_stats{};
AppMetrics last_dump_AppMetrics{};

void dump_metrics(unsigned long DUMP_UID){
/*
        unsigned long oTxns = 0, oCommits = 0;
        auto oTime = wallclock();

                //
                unsigned long nTime = wallclock();
                unsigned long nTxns = metrics->nTxns.load(std::memory_order_relaxed);
                unsigned long nCommits = metrics->nCommits.load(std::memory_order_relaxed);
                //
                unsigned long dTime = nTime - oTime;
                unsigned long dTxns = nTxns - oTxns;
                unsigned long dCommits = nCommits - oCommits;
                test_metrics_printd(RUN_UID,DUMP_UID,"MTxns",dTxns/1e6);
                test_metrics_printd(RUN_UID,DUMP_UID,"MCommits",dCommits/1e6);
                test_metrics_printd(RUN_UID,DUMP_UID,"Duration_s",dTime/1e9);
                test_metrics_print(RUN_UID,DUMP_UID,"Txns",dTxns);
                test_metrics_print(RUN_UID,DUMP_UID,"Commits",dCommits);
                test_metrics_print(RUN_UID,DUMP_UID,"Duration_ns",dTime);
                fflush(stdout);
                //
                oTxns = nTxns;
                oCommits = nCommits;
                oTime = nTime;
*/
        //TODO dump compile-time and runtime args too?

        int rc = ioctl(sivfs_mod_fd, SIVFS_GET_STATS, &sivfs_args);
        assert(rc == 0);

        metrics->nNanoseconds = wallclock();

        if (DUMP_UID == (unsigned long)-1){
                //DUMP_UID==-1 is used to initialize metrics but doesn't dump
                goto out;
        }

#define F(x) test_metrics_print( \
        RUN_UID, \
        DUMP_UID, \
        "SIVFSMetrics." #x, \
        sivfs_args.stats->x - last_dump_stats.x \
);

        SIVFS_STATS

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
        last_dump_stats = *sivfs_args.stats;
        last_dump_AppMetrics = *metrics;
}


int main(int argc, const char** argv){

        setup();

        for(size_t i = 0; i < 2; i++){
                int child = fork();
                if (child == 0){
                        thread_enter();
                        run(i);
                        //Exit very important here
                        exit(0);
                }
        }

        dump_metrics(-1);

        //Wake up the workers
        shared_space[0] = 1;

        for(size_t i = 0; i < 30; i++){
                sleep(1);

                unsigned long DUMP_UID = wallclock();
                dump_metrics(DUMP_UID);
        }

        //Hmm. We can actually exit here.
        shared_space[1] = 1;
}

