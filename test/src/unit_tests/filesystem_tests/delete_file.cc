void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);
        threadCount(1);
        timeout(10);

        //Automatically frees a reference.
        munmap(my_file.address, my_file.fsize);
        close(my_file.fhandle);
}

void run(){
        printf("In run, threadID %d\n", threadID);

#if TX_SYS == TX_SYS_FINE_GRAINED_LOCKING
        assert(0);
#endif

        //Something like a durability experiment. Verifies that closing and
        //opening the file again leaves writes intact.
        //
        //But really this is also useful for stressing the file open / file
        //close pathways (though this is rarely something that matters)
        size_t i;
        for(i = 0; ; i++){
                struct test_file my_file;
                uint64_t* x;

                my_file = open_test_file("file0", true, MAP_ADDR_0);
                x = (uint64_t*)my_file.address;
                try {
                        TM_WRITE(x + threadID, uint64_t(1));
                        //TM_WRITE(x + threadID, i);
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                bool aborted;
                TM_COMMIT_UPDATE(aborted);
                if (aborted){
                        assert(0);
                }
                munmap(my_file.address, my_file.fsize);
                //A ftruncate here will fail since we still have pages checked
                //out
                close(my_file.fhandle);

                my_file = open_test_file("file0", true, MAP_ADDR_0);
                x = (uint64_t*)my_file.address;
                try {
                        uint64_t x_check = TM_READ(x + threadID);
                        if (x_check){
                                throw std::runtime_error(
                                        "File was not zeroed on delete!"
                                );
                        }
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                TM_ABORT();

                munmap(my_file.address, my_file.fsize);
                close(my_file.fhandle);
        }
}
