#!/bin/bash

set -beEu -o pipefail

BINARY=$1

#Pass through args
argv=( $@ )
argc=${#argv[@]}
ARGS=${argv[@]:1:$argc}

make test

ISMOUNTED=$( ( mount | grep "/mnt/sivfs" || true ) | wc -l)

if [[ $ISMOUNTED == 0 ]]; then
./make_mount_run.sh
fi

./$BINARY $ARGS
