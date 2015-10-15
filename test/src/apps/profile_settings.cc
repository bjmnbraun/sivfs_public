//App benefits from blind writes

void setup(){
        //Allocate a bunch of profiles
}

void app_main(size_t thread_id){
        BENCHMARK_BEGIN
        for(size_t i = 0; i < NTXNS; i++){
                size_t user = random_user();
                size_t num_settings = random_num_settings();
                transaction {
                        settings* settings = get_settings(user);
                        for(size_t k = 0; k < num_settings; k++){
                                //Toggle those settings
                                size_t setting = random_setting();
                                //Can be blind
#if DO_BLIND_WRITES == 1
                                blind_write(&settings->settings[k], 1);
#else
                                settings->settings[k] = 1;
#endif
                        }
                }
        }
        BENCHMARK_END(NTXNS, "write_txn");
}
