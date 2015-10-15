#!/bin/bash

set -beEu -o pipefail

#Runs a command memory limited via cgroups.
#Example usage: ./run_memory_limited.sh 100M 120M "ls /"
#The first limit is a soft limit, the second a hard limit. It is more efficient
#to use a soft limit that is slightly lower than the hard limit, so that the
#kernel can reclaim pages over the soft limit in large bulk (otherwise it will
#fill up to the hard limit, and then free them in small groups - which is slow)

mem_soft_limit=$1
mem_limit=$2
command="$3"

# Create directory inside memory
CGROUPNAME="sivfs_limited"

if [ ! -d /sys/fs/cgroup/memory/$CGROUPNAME ]; then

cat <<EOF
===== Please add the following to your /etc/cgconfig.conf =====
group $CGROUPNAME {
        perm {
                admin {
                        uid = $USER;
                }
                task {
                        uid = $USER;
                }
        }
        memory {
        }
}
=====
and then restart the cgconfig service, i.e.
sudo systemctl restart cgconfig
EOF

exit 1

fi

echo $mem_soft_limit > \
/sys/fs/cgroup/memory/$CGROUPNAME/memory.soft_limit_in_bytes

echo $mem_limit > \
/sys/fs/cgroup/memory/$CGROUPNAME/memory.limit_in_bytes

# Execute the b-tree
# /usr/bin/time -v cgexec -g memory:bptgroup ./bt \
# $num_init $num_iter $print_tree # 2> res_${mem_limit}.out
cgexec -g memory:$CGROUPNAME "$command"
