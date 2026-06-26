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
echo "=> DUMPING VIDEO LOGIC FOR REVERSE ENGINEERING:"
echo "=========================================================="
echo "1. OSGLUXWN.c (X11 Image Creation)"
grep -B 2 -A 5 "XCreateImage" src/OSGLUXWN.c || true

echo -e "\n2. OSGLUXWN.c (Screen Update Function)"
grep -A 15 "Screen_OutputFrame" src/OSGLUXWN.c || true

echo -e "\n3. CNFGRAPI.h (Screen Macros)"
grep -i -E "screen|vram|macmem" src/CNFGRAPI.h || true

echo -e "\n4. GLOBGLUE.c (Screen Variables)"
grep -i -E "vMacScreen|MacMem" src/GLOBGLUE.c || true
echo "=========================================================="

echo "=> Diagnostic complete. Intentionally exiting."
exit 1
