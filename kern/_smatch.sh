#!/bin/bash

set -beEu -o pipefail

#JUST runs smatch
make clean

#Runs smatch on the kernel module to find some bugs
export RUN_SMATCH="Yes"
make
export RUN_SMATCH=
