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
echo "=> HOW DOES X11 ASSIGN THE VIDEO BUFFER?"
echo "=========================================================="
# Find exactly where X11 points its image data array
grep -n -C 2 "\->data" src/OSGLUXWN.c || true

echo "=========================================================="
echo "=> DUMPING ALL UNFILTERED GLOBALS FROM SCRNEMDV.c:"
echo "=========================================================="
# Compile just the screen emulator object using the host compiler to peek at its symbols
gcc -c src/SCRNEMDV.c -o SCRNEMDV.o -Icfg/ -Isrc/ -Os
nm SCRNEMDV.o | grep " [BD] " || true
echo "=========================================================="

exit 1
