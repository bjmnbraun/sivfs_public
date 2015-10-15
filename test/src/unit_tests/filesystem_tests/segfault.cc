void setup(){
        struct test_file my_file = open_test_file("file0", true, MAP_ADDR_0);
        threadCount(4);
        timeout(2);

        //You can do these in either order. An open mmap holds a reference on a
        //file handle, so the file is closed last in either order.
        close(my_file.fhandle);
        munmap(my_file.address, my_file.fsize);
}

void run(){
        printf("In run, threadID %d\n", threadID);

        struct test_file my_file = open_test_file("file0", false, MAP_ADDR_0);

        if (threadID == 2){
                //Segfault.
                ((uint64_t*)0xdead)[0]++;
        }
}
