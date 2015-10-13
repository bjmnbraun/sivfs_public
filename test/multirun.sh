#!/bin/bash

set -e

args="$@"
seq 4 | parallel -u ./a.out "$args"
