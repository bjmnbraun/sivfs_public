size_t incs_per_thread  = 0;

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

        const size_t million = 1000000;
        incs_per_thread = (5*million) / threadCount() + 1;
        //for performance tests, timeout is a failure.
        timeout(10);

        //Automatically frees a reference.
        munmap(my_file.address, my_file.fsize);
        close(my_file.fhandle);
}

size_t delta = 512;

void run(){
        struct test_file my_file = open_test_file("file0", false, MAP_ADDR_0);
        printf("In run, threadID %d\n", threadID);

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
        assert(0);
#endif

        uint64_t* x = (uint64_t*)my_file.address;

        size_t j = threadID;
        uint64_t* x_j = x + j * delta;

        size_t i;
        for(i = 0; i < incs_per_thread; i++){
                try {
                        TM_WRITE(x_j, *x_j + 1);
                } catch (const TM_ABORTED_EXCEPTION& e){
                        //Underflow well defined ?
                        i--;
                        continue;
                }
                bool aborted;
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        //Underflow well defined ?
                        i--;
                        continue;
                }
        }
}

//Important
#define TEST_CHECK
void check(){
        printf("In check!\n");

        struct test_file my_file = open_test_file("file0", false, MAP_ADDR_0);

        uint64_t* x = (uint64_t*)my_file.address;

        size_t j;
        for(j = 0; j < threadCount(); j++){
                uint64_t* x_j = x + j * delta;

                if (*x_j != incs_per_thread){
                        throw new std::runtime_error("Incorrect count value!");
                }
        }
}
