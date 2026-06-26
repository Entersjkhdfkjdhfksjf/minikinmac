#!/bin/bash
# build_kindle.sh

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=========================================================="
echo "=> UNMASKING THE VIDEO POINTER:"
echo "=========================================================="
echo "1. Exact source code of GetCurDrawBuff:"
grep -n -A 5 "LOCALFUNC ui3p GetCurDrawBuff" src/CONTROLM.h || true

echo -e "\n2. All globals exported by GLOBGLUE:"
# Compile just the glue file using the host compiler to peek at its symbols
gcc "src/GLOBGLUE.c" -o "GLOBGLUE.o" -c -Icfg/ -Isrc/ -Os || true
nm GLOBGLUE.o | grep -E " [BCDGRV] " || true
echo "=========================================================="

echo "=> Diagnostic complete. Intentionally exiting."
exit 1
