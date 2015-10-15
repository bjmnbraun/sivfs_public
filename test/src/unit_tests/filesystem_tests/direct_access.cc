void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);
        threadCount(1);
        timeout(10);

        //You can do these in either order. An open mmap holds a reference on a
        //file handle, so the file is closed last in either order.
        close(my_file.fhandle);
        munmap(my_file.address, my_file.fsize);
}

uint64_t* x;
size_t N = 4;

void _run(){
        size_t i;
        for(i = 0; i < 100; i++){
                try {
                        size_t j;
                        //Go backwards, because why not
                        for(j = N-1; ; j--){
                                TM_WRITE(x, i + 1);
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
}

void run(){
        printf("In run, threadID %d\n", threadID);

        struct test_file my_file = open_test_file("file0", false, MAP_ADDR_0);
        x = (uint64_t*)my_file.address;

        //Alternate between direct access and non-direct access

        uint64_t sum = 0;

        while(true){
                _run();
                TM_BEGIN_DIRECT_ACCESS();
                size_t j;
                //Go backwards, because why not
                for(j = N-1; ; j--){
                        x[j]++;
                        sum += x[j];
                        if (j == 0){
                                break;
                        }
                }
                assert(sum);
                TM_END_DIRECT_ACCESS();
        }
}
