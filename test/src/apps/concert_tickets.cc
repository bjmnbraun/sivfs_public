void setup(){
        //Allocate a bunch of seats
}

void app_main(){
        {
        BENCHMARK_BEGIN
        for(size_t i = 0; i < NTXNS; i++){
                bool attempted_once = false;
                transaction {
                        //If we fail, skip this txn
                        //TODO is there a more elegant way to do this
                        if (attempted_once){
                                abort_txn();
                        }
                        attempted_once = true;
                        size_t seat = random_seat();
                        size_t num_seats = random_num_seats();
                        bool fail = false;
                        for(size_t j = 0; j < num_seats; j++){
                                if (seat >= NSEATS){
                                        //not enough seats
                                        fail = true;
                                        break;
                                }
                                size_t* seat_taken = get_seat(seat);
                                if (*seat_taken){
                                        //seat taken
                                        fail = true;
                                        break;
                                }
                                *seat_taken = true;
                                //next seat
                                seat++;
                        }
                        if (fail){
                                //This may not be possible, ideally we support
                                //something like this
                                abort_txn();
                        }
                }
        }
        BENCHMARK_END(NTXNS, "write_txn");
        }
}
