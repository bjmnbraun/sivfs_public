void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);
        threadCount(1);
        timeout(10);

        //Automatically frees a reference.
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
        while(true){
                size_t nCoins = 1;
                size_t coins [nCoins];

                if (threadID == 0){
                        test_random(coins, sizeof(coins));
                        coins[0] %= 2;
                }

                try {
                        uint64_t x_check = TM_READ(x);
                        if (x_check){
                                printf("Value was: %zd\n", x_check);
                                throw std::runtime_error(
                                        "staged write was visible!"
                                );
                        }
                        if (threadID == 0){
                                TM_WRITE(x, uint64_t(1));
                                if (coins[0]){
                                        //Then cover the write
                                        TM_WRITE(x, (uint64_t)0);
                                }
                        }
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                if (threadID == 0 && coins[0]){
                        bool aborted;
                        TM_COMMIT_UPDATE(aborted);
                        if (aborted){
                                assert(0);
                        }
                } else {
                        //Otherwise abort deliberately here
                        TM_ABORT();
                }
        }
}
