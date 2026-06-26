#!/bin/bash
# build_kindle.sh

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching the generated Makefile..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile
sed -i 's/gcc /arm-linux-gnueabihf-gcc /g' Makefile
sed -i 's|-Isrc/|-Isrc/ -I/usr/arm-linux-gnueabihf/include|g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile
sed -i 's|-L/usr/X11R6/lib -lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile
sed -i 's|-lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile
sed -i 's/strip --strip-unneeded/arm-linux-gnueabihf-strip --strip-unneeded/g' Makefile

echo "=> Cross-compiling the emulator..."
make

echo "=========================================================="
echo "=> Dumping Memory Symbols to find the real VRAM array:"
echo "=========================================================="
arm-linux-gnueabihf-nm minivmac | grep -i -E "screen|vram" 
arm-linux-gnueabihf-nm minivmac | grep " B " | grep -i "mac"
echo "=========================================================="

echo "=> Build successful! Binary is ready for deployment."

