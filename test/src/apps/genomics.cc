

size_t setup(){
        //Allocate a queue of work items
}

void add_match(rule r, char* sequence_name, size_t offset){
        concurrent_vector<match> matches = get_matches(r);
        matches.push_back(match(sequence_name, offset));
}

void do_work_item(work_item* item){
        int fd = open(item->filename);
        seek(fd, item->offset);
        std::vector<char> current_vector;
        for(size_t i = 0; i < item->length; i++){
                current_vector.push_back(read(fd));
                if (current_vector.size() > 10){
                        current_vector.pop_front();
                }
                //Assuming reads are completely uninstrumented, the only writes
                //should be to shared memory
                transaction {
                        for(rule r : rules){
                                if (rule_matches(r, current_vector)){
                                        add_match(r, item->sequence, i);
                                }
                        }
                }
        }
        close(fd);
}

void get_status(){
        transaction {
                volatile size_t all_matches = 0;
                for(rule r : rules){
                        concurrent_vector<match>* matches = get_matches(r);
                        for(match m : matches){
                                all_matches+=m->offset;
                        }
                }
        }
}

void app_main(size_t thread_id){
        size_t itr = 0
        while(1){
                itr++;
                work_item* my_item = try_pull_work_item();
                if (my_item){
                        do_work_item(my_item);
                } else {
                        break;
                }

                if (thread_id == 0 && itr == 500){
                        BENCHMARK_BEGIN
                        for(size_t i = 0; i < NTXNS; i++){
                                get_status();
                        }
                }
        }


}
