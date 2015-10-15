#!/bin/bash

set -beEu -o pipefail

TEST_BIN=$1

#Pass through args
argv=( $@ )
argc=${#argv[@]}
TEST_ARGS=${argv[@]:1:$argc}

make stamp

ISMOUNTED=$( ( mount | grep "/mnt/sivfs" || true ) | wc -l)

if [[ $ISMOUNTED == 0 ]]; then
./make_mount_run.sh
fi

#TEST_DIR=/mnt/sivfs/$TEST_BIN
#mkdir -p $TEST_DIR

./$TEST_BIN $TEST_ARGS
