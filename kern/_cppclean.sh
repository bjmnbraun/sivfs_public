#!/bin/bash

set -beEu -o pipefail

mkdir -p tmp

#JUST runs cppclean on the source

#cppclean doesn't understand files that only define preprocessor directives
#so omit warnings about sivfs_config / sivfs_config_local
#
#Also, even if sivfs_common.h is unecessary including it is useful so that
#we have access to common defines if we decide to start using them, and always
#including this file is cheap
#
#Also, we include any userspace-facing structs from sivfs.h, so that
#userland code can just #include sivfs.h. cppclean picks up on these
#so we exclude:
#  - sivfs_stats.h
#
#Also, cppclean's checks for static data make sense for C++ codebases but in
#our context static variables are always defended by the order kernel modules
#are loaded in. So global static data is OK.
(cppclean . || true) > tmp/cppclean.out

(grep -vE "sivfs_config|sivfs_common.h|static data|sivfs_stats.h" tmp/cppclean.out || true) \
        | tee tmp/cppclean.out.filtered

if [[ -s tmp/cppclean.out.filtered ]]; then
        echo "There were warnings. Aborting."
        exit 1
fi
