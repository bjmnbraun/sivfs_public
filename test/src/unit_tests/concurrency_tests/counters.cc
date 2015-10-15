void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);
        threadCount(4);
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

        //Check atomicity of this many writes:
        size_t N = 64;
        //Space out the writes so we touch multiple pages
        size_t delta = 512;

        if (threadID == 0){
                size_t i;
                for(i = 0; ; i++){
                        try {
                                size_t j;
                                //Go backwards, because why not
                                for(j = N-1; ; j--){
                                        TM_WRITE(x + j * delta, i);
                                        if (j == 0){
                                                break;
                                        }
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
        } else {
                while(true){
                        bool aborted;
                        TM_COMMIT_UPDATE(aborted);
                        if (aborted){
                                assert(0);
                        }
                        size_t read_value;
                        try {
                                read_value = TM_READ(x);
                                size_t j;
                                for(j = 0; j < N; j++){
                                        if (TM_READ(x + j * delta) != read_value){
                                throw std::runtime_error(
                                        "Atomicity failed!\n"
                                );
                                        }
                                }
                        } catch (const TM_ABORTED_EXCEPTION& e){
                                assert(0);
                        }
                }
        }
}
