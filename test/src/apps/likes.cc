//Txn processing determined by "last seen" timestamp
//(i.e. profile staleness)
//Three settings - null = use master (nothing provided)
//-50 = 50 txns behind << can use a checkpoint 1/2 of the time
//-100 = 100 txns behind << can use a checkpoint
//-200 = 200 txns behind << can use a checkpoint
//
//Note that using a checkpoint may not be faster since it requires a full tlb
//flush... need to weigh this against the cost of the page faults + selectively
//flushing the working set at the end of the txn
//
//Checkpoints don't take advantage of when a page is written to in
//subsequent transactions (temporal locality). For this benchmark (likes)
//there is no temporal locality.
//
//This benchmark can take advantage of blind writes, after looking up the
//location for a like counter it can produce a write set including the
//increment only without fully instantiating the page

#define TEST_ADD_LIKE_TO_USER 0
#define TEST_INCREMENT_LIKE_COUNT 1

//Setup
void setup(){
#if WHICH_TEST == TEST_ADD_LIKE_TO_USER
        //Allocate a bunch of user like vectors
        //(should be "vector" merge mode)
#endif
#if WHICH_TEST == TEST_INCREMENT_LIKE_COUNT
        //Allocate a bunch of like counters
        //(should be "increment" merge mode)
#endif
}

void add_like_to_user(size_t user, size_t page){
        transaction {

        concurrent_vector<like_event> user_vec = get_like_vector(user);
        user_vec.add(like_event(page));

        }
}

void increment_like_count(size_t page){
        transaction {
                size_t* like_counter = get_like_count(page);
#if DO_BLIND_WRITES == 1
                blind_write(like_counter, 1);
#else
                (*like_counter)++;
#endif
        }
}

//Run on each of NTHREADS threads
void app_main(size_t thread_id){
        {
        BENCHMARK_BEGIN

        for(size_t i = 0; i < NTXNS; i++){
                size_t page = random();
#if WHICH_TEST == TEST_ADD_LIKE_TO_USER
                size_t user = random();
                add_like_to_user(user, page);
#endif
#if WHICH_TEST == TEST_INCREMENT_LIKE_COUNT
                //Eventual implication
                increment_like_count(page);
#endif
        }

        BENCHMARK_END(NTXNS, "write_txn");
        }

        {
        BENCHMARK_BEGIN
        //Even without saying, the engine should opportunistically treat us as
        //read-only and we should not have any page faults

        for(size_t i = 0; i < NTXNS; i++){
#if WHICH_TEST == TEST_ADD_LIKE_TO_USER
                
#endif
        }

        BENCHMARK_END(NTXNS, "read_txn");
        }
}
