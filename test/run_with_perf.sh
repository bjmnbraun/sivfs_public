#!/bin/bash

set -beEu -o pipefail

#Terminal signals and kill -9 are sent to whole process group, so we always
#kill the perf record on CTRL+C
sudo perf record -a --call-graph fp &

$1

#A bit heavy handed, but weird things happen when passing SIGTERM to processes
#that are under sudo.
sudo killall perf
