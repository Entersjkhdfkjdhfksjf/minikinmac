#!/bin/bash
# build_kindle.sh

set -e

# 1. Fetch FBInk using git with submodules to ensure i2c-tools is present
echo "=> Fetching FBInk repository (with submodules)..."
git clone --recurse-submodules https://github.com/NiLuJe/FBInk.git /tmp/FBInk-master
cd /tmp/FBInk-master

# 2. Compile ONLY the static library for Kindle (skips CLI utils)
echo "=> Compiling libfbink.a statically for Kindle..."
# Using CROSS_TC ensures FBInk uses the musl 'gcc', 'ar', and 'ranlib' tools
make CROSS_TC=armv7-unknown-linux-musleabihf KINDLE=1 staticlib

# Ensure the compiled library is available in the root folder for the linker
find . -name "libfbink.a" -exec cp {} . \;
cd -

# 3. Build the Mini vMac setup tool
echo "=> Compiling Mini vMac setup tool (host compiler)..."
gcc setup/tool.c -o setup_t

# 4. Generate configuration
echo "=> Generating core configuration..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

# 5. Patch the Makefile
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

# 6. Cross-compile
echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
