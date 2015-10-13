#!/bin/bash

set -v
set -beEu -o pipefail

./makeall.sh

#Check whether /sc/sivfskern is mounted
ISMOUNTED=$( ( mount | grep "sivfskern" || true ) | wc -l)
if [[ $ISMOUNTED == 0 ]]; then
        sudo mount --bind ~/Workspaces/sivfs/kern /sc/sivfskern
fi

pushd /sc/sivfskern > /dev/null
./mount-me.sh
popd > /dev/null
./a.out /mnt/sivfs/test
