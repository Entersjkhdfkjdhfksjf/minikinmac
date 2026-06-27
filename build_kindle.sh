#!/bin/bash
# build_kindle.sh

set -e

echo "=> Compiling Mini vMac setup tool (using host compiler)..."
# The host compiler in the container is typically x86_64-linux-gnu-gcc
gcc setup/tool.c -o setup_t

echo "=> Generating core configuration..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching Makefile to use the Koxtoolchain Kindle compiler..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# THE FIX: Use the official Kindle cross-compiler from the Koxtoolchain!
sed -i 's/gcc /arm-kindle-linux-gnueabi-gcc /g' Makefile

# FBInk is built into the Koxtoolchain sysroot, so we just link it normally
sed -i 's|-I/usr/X11R6/include||g' Makefile
sed -i 's|-L/usr/X11R6/lib -lX11|-lfbink -lm -ldl|g' Makefile
sed -i 's|-lX11|-lfbink -lm -ldl|g' Makefile

sed -i 's/strip --strip-unneeded/arm-kindle-linux-gnueabi-strip --strip-unneeded/g' Makefile

echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
