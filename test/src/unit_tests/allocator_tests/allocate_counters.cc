#include <stdexcept>

size_t million = 1000000;
//size_t counters_per_thread = 1 * million;
//size_t counters_per_thread = 10;
size_t counters_per_thread = 10000;
size_t incs_per_counter = 5;

std::atomic<uint64_t*>* xs = NULL;

void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);

        int argc;
        const char** argv;
        test_args(&argc, &argv);

        switch(argc){
        case 1:
                //default settings
                threadCount(4);
                break;
        case 2:
                {
                        int tc = atoi(argv[1]);
                        if (tc > 0){
                                threadCount(tc);
                                break;
                        }
                }
        default:
                throw new std::runtime_error("Invalid args");
        }

        //timeout is a failure.
        timeout(10);

        xs = (std::atomic<uint64_t*>*)mmap(
                NULL,
                counters_per_thread * threadCount() * sizeof(*xs),
                PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_SHARED,
                -1,
                0
        );
        assert(xs != MAP_FAILED);

        munmap(my_file.address, my_file.fsize);
        close(my_file.fhandle);
}

void use_free_counter(uint64_t* counter){
        //External consistency bites us here. We need to start a transaction,
        //i.e. call update, before reading.
        {
                bool aborted;
                TM_COMMIT_UPDATE(aborted);
                assert(!aborted);
        }

        //Use it
        size_t j;
        for(j = 0; j < incs_per_counter; j++){
                try {
                        TM_WRITE(counter, *counter + 1);
                        if (*counter != (j+1)){
                                uout(
                                        "Bad counter %p should: %zd is: %016lx",
                                        counter,
                                        j+1,
                                        *counter
                                );
                                assert(0);
                        }
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                bool aborted;
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        assert(0);
                }
        }

        //Free it
        try {
                TM_FREE(counter);
        } catch (const TM_ABORTED_EXCEPTION& e){
                assert(0);
        }
        bool aborted;
        TM_COMMIT_UPDATE(aborted);
        if (aborted){
                assert(0);
        }
}

void run(){
        struct test_file my_file = open_test_file("file0", false, MAP_ADDR_0);
        printf("In run, threadID %d\n", threadID);

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
        assert(0);
#endif

        auto push_queue = xs + counters_per_thread * threadID;

        auto pull_queue = xs + counters_per_thread * (threadID - 1);
        if (threadID == 0){
                pull_queue = NULL;
        }

        size_t i;
        for(i = 0; i < counters_per_thread; i++){
                //Alloc one and push it to the xs
                uint64_t* new_counter = NULL;
                try {
                        new_counter = (uint64_t*) TM_MALLOC(my_file.address, 0, sizeof(uint64_t));
                        assert(new_counter);
                        TM_WRITE(new_counter, uint64_t(0));
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                bool aborted;
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        assert(0);
                }
                push_queue[i].store(new_counter, std::memory_order_release);

                if (pull_queue){
                        while(!pull_queue[i].load(std::memory_order_acquire)){
                                cpu_relax();
                        }
                        //Use the other, then free it
                        use_free_counter(pull_queue[i]);
                }
        }
}
