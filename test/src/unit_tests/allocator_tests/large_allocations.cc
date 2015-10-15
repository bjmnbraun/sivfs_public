#include <stdexcept>

size_t large_alloc_min = 15000;
size_t large_alloc_max = 30000;
size_t repetitions = 100;

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

        munmap(my_file.address, my_file.fsize);
        close(my_file.fhandle);
}

void run(){
        struct test_file my_file = open_test_file("file0", false, MAP_ADDR_0);
        printf("In run, threadID %d\n", threadID);

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
        assert(0);
#endif

        size_t i;
        for(i = 0; i < repetitions; i++){
                //Alloc one and push it to the xs
                uint64_t* new_counter = NULL;
                size_t coin;
                test_random(&coin, sizeof(coin));

                size_t alloc_size = large_alloc_min;
                if (large_alloc_min != large_alloc_max){
                        alloc_size +=
                                coin 
                                % (large_alloc_max - large_alloc_min)
                        ;
                }

                alloc_size = ROUND_UP(alloc_size, sizeof(sivfs_word_t));

                uint64_t* space;
                try {
                        space = (uint64_t*) TM_MALLOC(my_file.address, 0, alloc_size);
                        assert(space);
                        TM_WRITE(space, uint64_t(42));
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                bool aborted;
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        assert(0);
                }

                if (i == repetitions - 1){
                        printf("Last allocation: %p\n", space);
                        //stall a bit here to avoid duplicates
                        printf("Sleeping...\n");
                        sleep(1);
                }

                //Free it
                try {
                        TM_FREE(space);
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        assert(0);
                }
        }
}
