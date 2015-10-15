#!/bin/bash

#Look up source code lines output by kernel crash dumps in the kernel module
make

mycommands=$(mktemp)
cat > $mycommands << EOF
list *($1)
EOF
gdb -batch -x $mycommands sivfs.ko
rm $mycommands
