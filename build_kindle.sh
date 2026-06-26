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
echo "=> TRACING THE X11 VRAM ASSIGNMENT:"
echo "=========================================================="
echo "1. Context around the_data:"
grep -n -B 20 -A 5 "my_image->data = the_data" src/OSGLUXWN.c || true

echo -e "\n2. Where is image_Mem1 declared?"
grep -n -B 2 -A 2 "image_Mem1" src/OSGLUXWN.c || true
echo "=========================================================="

exit 1

