#include "metrics.h"

bool stdout_readied = false;
void test_ready_stdout(){
        setlinebuf(stdout);
        stdout_readied = true;
}
