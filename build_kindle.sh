#!/bin/bash
# build_kindle.sh

set -e

# 1. Fetch and compile FBInk from source using the musl cross-compiler
echo "=> Fetching FBInk source code..."
curl -L https://github.com/NiLuJe/FBInk/archive/refs/heads/master.tar.gz | tar xz -C /tmp
cd /tmp/FBInk-master

echo "=> Compiling libfbink.a statically..."
make CC=armv7-unknown-linux-musleabihf-gcc static
cd -

# 2. Build the Mini vMac setup tool using the native Ubuntu host compiler
echo "=> Compiling Mini vMac setup tool (host compiler)..."
gcc setup/tool.c -o setup_t

# 3. Generate configuration
echo "=> Generating core configuration..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

# 4. Patch the Makefile
echo "=> Patching Makefile for Musl Static Hardware Build..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# INJECT YOUR EXACT LLAMA.CPP MUSL FLAGS
sed -i 's/gcc /armv7-unknown-linux-musleabihf-gcc -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static /g' Makefile

# Tell GCC where to find the FBInk headers we just downloaded
sed -i 's|-Isrc/|-Isrc/ -I/tmp/FBInk-master|g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile

# Link against our newly compiled static libfbink.a
sed -i 's|-L/usr/X11R6/lib -lX11|-L/tmp/FBInk-master -lfbink -lm|g' Makefile
sed -i 's|-lX11|-L/tmp/FBInk-master -lfbink -lm|g' Makefile

# Ensure we use the musl strip tool
sed -i 's/strip --strip-unneeded/armv7-unknown-linux-musleabihf-strip --strip-unneeded/g' Makefile

# 5. Cross-compile
echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
