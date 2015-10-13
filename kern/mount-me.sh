#!/bin/bash

#For debugging:
#set -v

set -beEu -o pipefail

ISMOUNTED=$( ( mount | grep "/mnt/sivfs" || true ) | wc -l)

if [[ $ISMOUNTED == 1 ]]; then
echo "Unmounting sivfs..."
sudo umount /mnt/sivfs
#Seems that unmounting is slightly asynchronous...
sleep 2
fi

echo "Building sivfs and installing module..."
sudo make install > /dev/null
echo "Mounting example filesystem (/mnt/sivfs)..."
sudo mount -t sivfs none /mnt/sivfs
sudo chmod ugo+rw /mnt/sivfs
