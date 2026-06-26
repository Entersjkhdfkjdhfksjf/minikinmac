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
echo "=> DUMPING THE EXACT MACRO DEFINITIONS:"
echo "=========================================================="
echo "[GetCurDrawBuff]"
grep -rn "GetCurDrawBuff" src/ || true

echo -e "\n[vMacScreen]"
grep -rn "vMacScreen" src/ || true
echo "=========================================================="

exit 1
