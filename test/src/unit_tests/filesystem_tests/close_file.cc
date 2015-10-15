void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);
        threadCount(1);
        timeout(10);

        //You can do these in either order. An open mmap holds a reference on a
        //file handle, so the file is closed last in either order.
        close(my_file.fhandle);
        munmap(my_file.address, my_file.fsize);
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
        for(i = 0; i < 1; i++){
                struct test_file my_file;
                uint64_t* x;

                my_file = open_test_file("file0", false, MAP_ADDR_0);
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
                //A ftruncate here will fail since we still have pages checked
                //out
                close(my_file.fhandle);

                //Page fault here is OK!
                printf("Value is: %lu\n", TM_READ(x + (i+1) * 512));

                munmap(my_file.address, my_file.fsize);

                my_file = open_test_file("file0", false, MAP_ADDR_0);
                x = (uint64_t*)my_file.address;
                try {
                        uint64_t x_check = TM_READ(x + threadID);
                        assert(x_check);
                } catch (const TM_ABORTED_EXCEPTION& e){
                        assert(0);
                }
                TM_ABORT();

                munmap(my_file.address, my_file.fsize);
                close(my_file.fhandle);
        }
}
