#!/bin/bash

#For debugging:
#set -v

set -beEu -o pipefail

ISMOUNTED=$( ( mount | grep "/mnt/sivfs" || true ) | wc -l)

sudo mkdir -p /mnt/sivfs

if [[ $ISMOUNTED == 1 ]]; then
echo "Unmounting sivfs..."
sudo umount /mnt/sivfs
#Seems that unmounting is slightly asynchronous...
fi

ISMOUNTED=$( ( mount | grep "/mnt/sivfs" || true ) | wc -l)
if [[ $ISMOUNTED == 1 ]]; then
echo "Couldn't unmount sivfs!"
exit 1
fi

ISLOADED=$( ( lsmod | grep "sivfs" || true ) | wc -l)

if [[ $ISLOADED == 1 ]]; then
echo "Unloading sivfs..."
sudo rmmod sivfs
#Seems that unloading is slightly asynchronous...
fi

#Try again
ISLOADED=$( ( lsmod | grep "sivfs" || true ) | wc -l)
if [[ $ISLOADED == 1 ]]; then
echo "Couldn't unload sivfs!"
exit 1
fi

echo "Building sivfs and installing module..."
configfile=sivfs_config_local.h
if [ ! -e $configfile ] ; then
        #Don't touch if it exists as that will force a recompile
        touch $configfile
fi

configfile=sivfs_config_local.h
if [ ! -e $configfile ] ; then
        #Don't touch if it exists as that will force a recompile
        touch $configfile
fi

make
# > /dev/null
sudo make install > /dev/null

echo "Mounting example filesystem (/mnt/sivfs)..."
sudo mkdir -p /mnt/sivfs
#sudo mount -t sivfs none /mnt/sivfs
sudo mount -t sivfs tmpfs /mnt/sivfs
sudo chmod ugo+rw /mnt/sivfs
