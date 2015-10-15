#!/bin/bash

set -beEu -o pipefail

# Makes documentation and diagrams using doxygen

if [[ ! -e Doxyfile ]]; then
        echo "Need to run doxywizard or such to generate Doxyfile first"
        exit 1
fi

doxygen

$BROWSER $(pwd)/doc/html/index.html
