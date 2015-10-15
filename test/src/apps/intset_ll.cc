#include <sys/mman.h>
#include <fcntl.h>
#include <vector>

#include "txsys.h"
#include "timers.h"
#include "metrics.h"
#include "test_random.h"
#include "config.h"

volatile char* shared_space;
const size_t shared_space_size = 128000;

int intset_fhandle;

const char* intset_fname = "/mnt/sivfs/intset";
void* const intset_address = (void*)(0xE00ULL<<32ULL);

//= 64 - 16 bytes so that node_t is cache line aligned
struct value_t {
        uint64_t words[6];
};

struct node_t {
        struct node_t* prev;
        struct node_t* next;
        struct value_t value;
};

typedef struct node_t list_head_t;

typedef struct {
        list_head_t* __head;
        struct node_t* node;
} intset_iter_t;

HEADER_INLINE int intset_compare_nodes(struct node_t* x, struct node_t* y){
        uint64_t xv = TM_READ(&x->value.words[0]);
        uint64_t yv = TM_READ(&y->value.words[0]);
        if (xv == yv){
                return 0;
        } else if (xv < yv){
                return -1;
        } else {
                return 1;
        };
}

HEADER_INLINE void intset_iter_advance(intset_iter_t* iter){
        struct node_t* node = iter->node;
        struct node_t* next = TM_READ(&node->next);
        if (next == iter->__head){
                //Reached end.
                iter->node = NULL;
                return;
        }
        iter->node = next;
}

#define intset_for_each(head,iter) \
        for((iter)->__head = head, (iter)->node=head, intset_iter_advance(iter); \
        (iter)->node; \
        intset_iter_advance(iter))

//Adds x after target
HEADER_INLINE void intset_add_after(struct node_t* x, struct node_t* target){
        //The node to add x before
        struct node_t* rtarget = TM_READ(&target->next);
        TM_WRITE(&target->next, x);
        TM_WRITE(&rtarget->prev, x);
        TM_WRITE(&x->next, rtarget);
        TM_WRITE(&x->prev, target);

        //Need to write promote both rtarget and target to conflict with removes
}

//Removes x
HEADER_INLINE void intset_remove(struct node_t* x){
        //Get left and right neighbors of x
        struct node_t* target = TM_READ(&x->prev);
        struct node_t* rtarget = TM_READ(&x->next);
        //Invalidate x. Note that we don't clear x's value. We don't need to
        //since privatization of x will conflict with any other remove.
        TM_WRITE(&target->next, rtarget);
        TM_WRITE(&rtarget->prev, target);
        TM_WRITE(&x->next, nullptr);
        TM_WRITE(&x->prev, nullptr);

        //Need to write promote both rtarget and target to conflict with removes
}

HEADER_INLINE void intset_find(
        /*out*/ struct node_t** _found,
        /*out*/ bool* _exact_match,
        list_head_t* head,
        uint64_t toFind
){
        //Find last element smaller than i
        struct node_t* found = head;
        bool exact_match = false;

        intset_iter_t iter;
        intset_for_each(head, &iter){
                uint64_t value = TM_READ(&iter.node->value.words[0]);
                if (value > toFind){
                        //We went too far.
                        break;
                }
                if (value == toFind){
                        exact_match = true;
                }
                found = iter.node;
        }

//Return results
        *_found = found;
        *_exact_match = exact_match;
}

HEADER_INLINE size_t intset_get_size(
        list_head_t* head
){
        size_t toRet = 0;
        intset_iter_t iter;
        intset_for_each(head, &iter){
                toRet++;
        }

        return toRet;
}

static void checkNode() {
        static_assert(sizeof(node_t) == 64, "sizeof(node_t)!=64");
};

/*
#define READ_PERCENTAGE APP_ARG_VALUE(30)
#define ADD_PERCENTAGE APP_ARG_VALUE(40)
#define REMOVE_PERCENTAGE APP_ARG_VALUE(30)
*/
#define WRITER_THREAD_PERCENTAGE APP_ARG_VALUE(50)
#define ADD_PERCENTAGE APP_ARG_VALUE(60)
#define REMOVE_PERCENTAGE APP_ARG_VALUE(40)

size_t nThreads = 4;

#define APP_ARGS \
        F(WRITER_THREAD_PERCENTAGE) \
        F(ADD_PERCENTAGE) \
        F(REMOVE_PERCENTAGE) \

        //F(READ_PERCENTAGE)

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

const size_t nOpsPerTxn = 1;

//This many nodes will be split among the threads
//const size_t intset_nNodes = 10000;
const size_t intset_nNodes = 256;
//const size_t intset_nNodes = nThreads * 1;

//const size_t intset_size = intset_nRecords * RECORD_SIZE;
#define NODE_SIZE sizeof(struct node_t)

//Pointer to sequential array of intset_nNodes many nodes
struct node_t* intset_nodes = NULL;

//Pointer to intset head
list_head_t* intset_head = NULL;

//const size_t intset_size = intset_nNodes * NODE_SIZE + NODE_SIZE;
const size_t intset_size = 0x100ULL << 32ULL; //db_nRecords * RECORD_SIZE;

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
        printf("About to allocate %f GB\n", intset_size / (double)(1ULL << 30));

        int fhandle = open(intset_fname, O_RDWR | O_TRUNC | O_CREAT, 0600);
        assert(fhandle >= 0);
        int rc = ftruncate(fhandle, intset_size);
        assert(rc == 0);
        close(fhandle);

}
#elif TX_SYS == TX_SYS_GL
void setup_gl(){
        int mapflags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                intset_address,
                intset_size,
                PROT_READ | PROT_WRITE,
                mapflags,
		-1,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == intset_address);

        intset_nodes = (struct node_t*)ptr;
        ptr = ((char*)ptr + intset_nNodes * NODE_SIZE);
        intset_head = (list_head_t*)ptr;

        gl_init((char*)shared_space + 4096, shared_space_size - 4096);
}
#elif TX_SYS == TX_SYS_LOCKTABLE
void setup_locktable(){
        int mapflags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                intset_address,
                intset_size,
                PROT_READ | PROT_WRITE,
                mapflags,
		-1,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == intset_address);

        intset_nodes = (struct node_t*)ptr;
        ptr = ((char*)ptr + intset_nNodes * NODE_SIZE);
        intset_head = (list_head_t*)ptr;

        locktable_init((char*)shared_space + 4096, shared_space_size - 4096);
}
#else
        //Do nothing. We will assert in main.
#endif

void thread_enter_sivfs(){
        int fhandle = open(intset_fname, O_RDWR);
        assert(fhandle >= 0);
        //??? Is this necessary?
        int mapflags = MAP_SHARED | MAP_FIXED;
        void* ptr = mmap(
                intset_address,
                intset_size,
                PROT_READ | PROT_WRITE,
                mapflags,
                fhandle,
                0
        );
        assert(ptr != MAP_FAILED);
        assert(ptr == intset_address);

        intset_nodes = (struct node_t*)ptr;
        ptr = ((char*)ptr + intset_nNodes * NODE_SIZE);
        intset_head = (list_head_t*)ptr;
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

        //Thread id 0 prepares the head
        if (id == 0){
                bool aborted;
                TM_COMMIT_UPDATE(aborted);

                TM_WRITE(&intset_head->next, intset_head);
                TM_WRITE(&intset_head->prev, intset_head);

                TM_COMMIT_UPDATE(aborted);
                assert(!aborted);
                printf("list head initialized\n");
                shared_space[2] = 2;
        }

        WAIT(shared_space + 2, 2);

        //Prepare my nodes
        std::vector<struct node_t*> nodes;
        size_t nodesPerThread = intset_nNodes / nThreads;
        size_t nodesOffset = id * nodesPerThread;
        for(size_t i = 0; i < nodesPerThread; i++){
                nodes.push_back(intset_nodes + i + nodesOffset);
        }

        bool aborted;

        bool shouldWrite = (
                id < WRITER_THREAD_PERCENTAGE * nThreads / 100
        );

        //XXX hack
        sleep(1);

        TM_BATCH_END();

        if (shouldWrite){
                sleep(1);
        }

        std::vector<struct node_t*> added, removed;
        while(true){
                size_t coins [nOpsPerTxn * 2];
                test_random(coins, sizeof(coins));

                retry:
                try {

#if 1
                for(size_t j = 0; j < nOpsPerTxn; j++){
                        size_t i = coins[j*2+0];
                        size_t opSelect = coins[j*2+1]%100;

                        /*
                        bool shouldRead = opSelect < READ_PERCENTAGE;
                        //Underflow desired
                        opSelect -= READ_PERCENTAGE;
                        */

                        bool shouldAdd = shouldWrite && opSelect < ADD_PERCENTAGE;
                        opSelect -= ADD_PERCENTAGE;
                        bool shouldRemove = shouldWrite && opSelect < REMOVE_PERCENTAGE;

                        if (!shouldWrite + shouldAdd + shouldRemove != 1){
                                assert(0);
                        }

                        struct node_t* found;
                        bool exact_match;

                        intset_find(&found, &exact_match, intset_head, i);

                        if (shouldAdd){
                                if (!nodes.empty() && !exact_match){
                                        struct node_t* toAdd = nodes.back();
                                        nodes.pop_back();
                                        added.push_back(toAdd);
                                        //Write the value
                                        //This might throw an exception!
                                        TM_WRITE(&toAdd->value.words[0], i);
                                        intset_add_after(toAdd, found);
                                }
                        }

                        if (shouldRemove){
                                if (found == intset_head){
                                        found = TM_READ(&intset_head->prev);
                                        if (!found){
                                                //Should never happen
                                                assert(0);
                                        }
                                }

                                if (found == intset_head){
                                        //Empty list
                                } else {
                                        intset_remove(found);
                                        removed.push_back(found);
                                }
                        }
                }
#else
                //A bad interleaving.
                //T1: Add 2
                //T2: Add 4
                //T3: Remove 4
                //T4: Add 3, then scan for 5
                //T5: (ON same thread as T3) Reuse node to add 1
                //Depending on the interleaving of T3-T5 T4 might get
                //caught in a cycle

                size_t i = coins[j*2+0];
                size_t opSelect = coins[j*2+1]%100;
                bool shouldRead = opSelect < READ_PERCENTAGE;
                //Underflow desired
                opSelect -= READ_PERCENTAGE;
                bool shouldAdd = opSelect < ADD_PERCENTAGE;
                opSelect -= ADD_PERCENTAGE;
                bool shouldRemove = opSelect < REMOVE_PERCENTAGE;

                if (shouldRead + shouldAdd + shouldRemove != 1){
                        assert(0);
                }
#endif

                } catch (const TM_ABORTED_EXCEPTION& e){
                }
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        //Undo added / removed
                        for(auto itr = added.begin(); itr != added.end(); ++itr){
                                nodes.push_back(*itr);
                        }
                        added.clear();
                        removed.clear();
                        goto retry;
                } else {
                        //Commit added / removed
                        for(auto itr = removed.begin(); itr != removed.end(); ++itr){
                                nodes.push_back(*itr);
                        }
                        added.clear();
                        removed.clear();
                }

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
        printf("%zd left!\n", nodes.size());
        TM_COMMIT_UPDATE(aborted);
        printf("%zd size!\n", intset_get_size(intset_head));
        TM_COMMIT_UPDATE(aborted);
}

tx_sys_stats_t last_dump_tx_sys_stats{};
AppMetrics last_dump_AppMetrics{};

void dump_metrics(unsigned long DUMP_UID){
        //TODO dump compile-time and runtime args too?

	tx_sys_get_stats(tx_sys_stats);

        metrics->nNanoseconds = wallclock();

        if (DUMP_UID == (unsigned long)-1){
                test_ready_stdout();
		//First dump. Dump args.
		txsys_compile_args_print(RUN_UID);

                const char** argName = APP_ARG_NAMES;
                const char** argValue = APP_ARG_VALUES;
#define F(x) test_args_print( \
        RUN_UID, \
        "AppArgs." #x, \
        x \
);

        F(WRITER_THREAD_PERCENTAGE)
       // F(READ_PERCENTAGE)
        F(ADD_PERCENTAGE)
        F(REMOVE_PERCENTAGE)
        F(nThreads)

#undef F

#define F(x) test_args_prints( \
        RUN_UID, \
        "AppArgs." #x, \
        x \
);

        //Oh, whatever. Call this linked list.
        const char* binary_name = "linked list";

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

        for(size_t i = 0; i < nThreads; i++){
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

        sleep(1);
}

