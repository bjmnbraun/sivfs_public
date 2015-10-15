#!/bin/bash

set -beEu -o pipefail

#Runs sudo gdb -p {pid} for each {pid} in arguments, in a separate window, in a
#new tmux session.

#Run these commands after launching
export Cfile=$(mktemp)

trap "rm $Cfile" EXIT

cat >$Cfile <<EOF
c
EOF

tmux new "$(for i in $@; do echo "tmux neww 'sudo gdb -p $i -x $Cfile'"; done)"
