std::atomic<uint64_t>* external_test;

void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);
        printf("In setup\n");
        threadCount(4);
        timeout(10);

        external_test = (std::atomic<uint64_t>*)mmap(
                NULL,
                4096,
                PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_SHARED,
                -1,
                0
        );
        assert(external_test != MAP_FAILED);

        munmap(my_file.address, my_file.fsize);
        close(my_file.fhandle);
}

void run(){
        struct test_file my_file = open_test_file("file0", false, MAP_ADDR_0);
        printf("In run, threadID %d\n", threadID);

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
                assert(0);
#endif

        uint64_t* x = (uint64_t*)my_file.address;
        if (threadID == 0){
                size_t i;
                for(i = 0; ; i++){
                        try {
                                TM_WRITE(x, i);
                        } catch (const TM_ABORTED_EXCEPTION& e){
                                assert(0);
                        }
                        bool aborted;
                        TM_COMMIT_UPDATE(aborted);
                        if (aborted){
                                assert(0);
                        }
                        *external_test = i;
                }
        } else {
                while(true){
                        size_t read_value_check = external_test->load(
                                std::memory_order_acquire
                        );
                        bool aborted;
                        TM_COMMIT_UPDATE(aborted);
                        if (aborted){
                                assert(0);
                        }
                        size_t read_value;
                        try {
                                read_value = TM_READ(x);
                        } catch (const TM_ABORTED_EXCEPTION& e){
                                assert(0);
                        }
                        if (read_value < read_value_check){
                                fprintf(stderr, "External consistency failed!\n");
                                throw std::runtime_error(
                                        "External consistency failed!\n"
                                );
                        }
                }
        }
}
