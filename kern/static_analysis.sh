#!/bin/bash

set -beEu -o pipefail

#Runs static analysis on the kernel module using smatch and cppclean
#Actual invocation delegated to helper scripts below
./_smatch.sh
./_cppclean.sh
