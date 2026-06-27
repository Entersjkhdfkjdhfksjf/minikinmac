#!/bin/bash
# build_kindle.sh

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating core configuration..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching Makefile with exact Cortex-A9 Static Hardware Flags..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# INJECT YOUR LLAMA.CPP FLAGS AND FORCE FULLY STATIC LINKING
sed -i 's/gcc /arm-linux-gnueabihf-gcc -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static /g' Makefile

sed -i 's|-Isrc/|-Isrc/ -I/usr/arm-linux-gnueabihf/include|g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile

# Link FBInk statically alongside the static C library
sed -i 's|-L/usr/X11R6/lib -lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile
sed -i 's|-lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile

sed -i 's/strip --strip-unneeded/arm-linux-gnueabihf-strip --strip-unneeded/g' Makefile

echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."

