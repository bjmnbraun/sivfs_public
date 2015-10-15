#include <sys/mman.h>
#include <fcntl.h>
#include <vector>

#include "txsys.h"
#include "timers.h"
#include "metrics.h"
#include "test_random.h"
#include "config.h"

/*
 * Stresses the workingset cache by having multiple threads, each of which
 * iterate over a large workingset modifying a single page at a time.
 *
 * Can be run in a cgroup to test memory restrictions, processes should be
 * unable to go outside their restriction.
 *
 * Perhaps we can have the cache react to OS memory pressure? That would be
 * cool.
 */
volatile char* shared_space;
const size_t shared_space_size = 128000;

int large_array_fhandle;

const char* large_array_fname = "/mnt/sivfs/cache_stress";
void* const large_array_address = (void*)(0xE00ULL<<32ULL);

#define NTHREADS APP_ARG_VALUE(1)

//1GB. Should fit fine on any computer, but if threads start accumulating
//workingsets on that order we run out of memory.
#define ARRAY_SIZE_MB APP_ARG_VALUE(1000)
//#define ARRAY_SIZE_MB APP_ARG_VALUE(1)

#define APP_ARGS \
        F(NTHREADS) \
        F(ARRAY_SIZE_MB) \

//Don't modify the next few lines...

#define F(X) #X,
static const char* APP_ARG_NAMES[] = {
	APP_ARGS
};
#undef F

#define F(X) X
#define APP_ARG_VALUE(X) #X,
static const char* APP_ARG_VALUES[] = {
	APP_ARGS
};
#undef APP_ARG_VALUE
#undef F

//Default _VALUE to identity
#define APP_ARG_VALUE(X) X

//End "Don't modify the next few lines"

//Pointer to big array
uint64_t* large_array = NULL;

const size_t large_array_size = ((size_t)ARRAY_SIZE_MB)*(1<<20ULL);
const size_t large_array_nentries = large_array_size / sizeof(*large_array);
const size_t PGSIZE = 4096;
const size_t ENTRIES_PER_PAGE = PGSIZE / sizeof(*large_array);


unsigned long RUN_UID = -1;

#define APP_METRIC(x) F(x)

#define APP_METRICS\
        APP_METRIC(nNanoseconds) \

//Don't modify below this
class AppMetrics {
  public:
#define F(x) uint64_t x;
        APP_METRICS
#undef F
};
AppMetrics* metrics = NULL;

tx_sys_stats_t* tx_sys_stats = NULL;
//End don't modify below this

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
        printf("About to allocate %f GB\n", large_array_size / (double)(1ULL << 30));

        int fhandle = open(large_array_fname, O_RDWR | O_TRUNC | O_CREAT, 0600);
        assert(fhandle >= 0);
        int rc = ftruncate(fhandle, large_array_size);
        assert(rc == 0);
        close(fhandle);

}
#elif TX_SYS == TX_SYS_GL
void setup_gl(){
        int mapflags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                large_array_address,
                large_array_size,
                PROT_READ | PROT_WRITE,
                mapflags,
		-1,
                0
        );
        assert(rc == 0);
        assert(ptr != MAP_FAILED);
        assert(ptr == large_array_address);

        large_array = (uint64_t*)ptr;

        gl_init((char*)shared_space + 4096, shared_space_size - 4096);
}
#elif TX_SYS == TX_SYS_LOCKTABLE
void setup_locktable(){
        int mapflags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                large_array_address,
                large_array_size,
                PROT_READ | PROT_WRITE,
                mapflags,
		-1,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == large_array_address);

        large_array = (uint64_t*)ptr;

        locktable_init((char*)shared_space + 4096, shared_space_size - 4096);
}
#else
        //Do nothing. We will assert in main.
#endif

void thread_enter_sivfs(){
        int fhandle = open(large_array_fname, O_RDWR);
        assert(fhandle >= 0);
        //??? Is this necessary?
        int mapflags = MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                large_array_address,
                large_array_size,
                PROT_READ | PROT_WRITE,
                mapflags,
                fhandle,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == large_array_address);

        large_array = (uint64_t*)ptr;
}

void thread_enter_gl(){
}

void thread_enter_locktable(){
}

void run(size_t id){

        size_t offset = 0;
        while(true){
                retry:
                try {

                //This is wrong since it's just a write without instrmentation
                //- how do we detect this?
                //TODO script that runs the app in a way that uninstrumented
                //writes are found.
                //large_array[offset]++;
                TM_WRITE(&large_array[offset], TM_READ(&large_array[offset])+1);
                //((volatile uint64_t*)large_array)[offset];

                } catch (const TM_ABORTED_EXCEPTION& e){
                }
                bool aborted;
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        goto retry;
                }

                //Break condition
                if (shared_space[1] == 1){
                        break;
                }

                //Advance
                size_t noffset = offset + ENTRIES_PER_PAGE;
                if (noffset >= large_array_nentries){
                        offset = 0;
                } else {
                        offset = noffset;
                }
        }

        printf("Exiting!\n");
}

tx_sys_stats_t last_dump_tx_sys_stats{};
AppMetrics last_dump_AppMetrics{};

void dump_metrics(unsigned long DUMP_UID){
        //TODO dump compile-time and runtime args too?

	tx_sys_get_stats(tx_sys_stats);

        metrics->nNanoseconds = wallclock();

        if (DUMP_UID == (unsigned long)-1){
                //Needed before printing any args or metrics
                test_ready_stdout();

		//First dump. Dump args.
		txsys_compile_args_print(RUN_UID);

                const char** argName = APP_ARG_NAMES;
                const char** argValue = APP_ARG_VALUES;
#define F(X) \
                test_compile_arg_print(RUN_UID, *argName, *argValue); \
                argName++;\
                argValue++;

                APP_ARGS
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
#if TX_SYS_INCONSISTENT_READS
        printf(
                "This benchmark "
                __FILE__
                " requires consistent reads during txn"
                "\n"
        );
        return 0;
#endif

        setup_all();

#if TX_SYS == TX_SYS_SIVFS
        setup_sivfs();
#elif TX_SYS == TX_SYS_GL
	setup_gl();
#elif TX_SYS == TX_SYS_LOCKTABLE
        setup_locktable();
#else
        assert(0);
#endif

        for(size_t i = 0; i < NTHREADS; i++){
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
                        //Side project - can we detect the absence of fork here
                        //(which would indicate a forkbomb?)
                        exit(0);
                }
        }

        dump_metrics(-1);

        for(size_t i = 0; i < 15; i++){
                sleep(1);

                unsigned long DUMP_UID = wallclock();
                dump_metrics(DUMP_UID);
        }

        //Hmm. We can actually exit here.
        shared_space[1] = 1;

        //Unnecessary, but avoids print messages happening after the terminal
        //prints out a shell prompt
        sleep(1);
}

