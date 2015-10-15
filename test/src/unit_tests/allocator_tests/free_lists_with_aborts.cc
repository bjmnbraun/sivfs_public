#include <stdexcept>

size_t objsize = 400;
size_t repetitions = 1000;

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
                break;
        default:
                throw new std::runtime_error("Invalid args");
        }

        //timeout is a failure.
        timeout(2);

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
                size_t coin;
                test_random(&coin, sizeof(coin));

                #define ABORT_PERCENT 50

                uint64_t* space;
                try {
                        space = (uint64_t*) TM_MALLOC(my_file.address, 0, objsize);
                        assert(space);
                        TM_WRITE(space, uint64_t(42));
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }

                if ((coin % 100) < ABORT_PERCENT){
                        TM_ABORT();
                } else {
                        bool aborted;
                        TM_COMMIT_UPDATE(aborted);
                        if (aborted){
                                assert(0);
                        }
                }
        }

        printf("Done!\n");
}
