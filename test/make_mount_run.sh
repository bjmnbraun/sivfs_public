#!/bin/bash

set -beEu -o pipefail

#Superuser must have access to these directories before running,
#otherwise you will get a permisison denied on make install

#MAIN_ARGS=$1

#0 just runs the default sanity test
MAIN_ARGS=0

configfile=src/txsys_config_local.h
if [ ! -e $configfile ] ; then
        #Don't touch if it exists as that will force a recompile
        touch $configfile
fi

#Not necessary but convenient when editing test code to fail here
make

#Check whether /aliases/sivfskern is mounted, aka have we made a shortcut yet
ISMOUNTED=$( ( mount | grep "sivfskern" || true ) | wc -l)
if [[ $ISMOUNTED == 0 ]]; then
        #"Shortcut" directory under /aliases/ ...
        sudo mkdir -p /aliases/sivfskern
        sudo mount --bind ~/Workspaces/sivfs/kern /aliases/sivfskern
fi

pushd /aliases/sivfskern > /dev/null
./mount-me.sh
popd > /dev/null

#Kernel headers may have changed
make

set -v
./bin/src/main /mnt/sivfs/test MAIN_ARGS
