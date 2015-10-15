#!/bin/bash

set -beEu -o pipefail

grep -rin "$1" .
