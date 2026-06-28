#!/bin/bash
# build_kindle.sh

set -e

echo "=> Fetching FBInk repository (with submodules)..."
git clone --recurse-submodules https://github.com/NiLuJe/FBInk.git /tmp/FBInk-master
cd /tmp/FBInk-master

echo "=> Compiling libfbink.a statically for Kindle..."
make CROSS_TC=armv7-unknown-linux-musleabihf KINDLE=1 staticlib
find . -name "libfbink.a" -exec cp {} . \;
cd -

echo "=> Compiling Mini vMac setup tool (host compiler)..."
gcc setup/tool.c -o setup_t

echo "=> Generating core configuration..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching Makefile for Musl Static Hardware Build..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# THE FIX: Inject ARM hardware safety flags!
# -mno-unaligned-access : Forces GCC to compile safe memory casts (prevents 68k Segfaults)
# -fno-strict-aliasing : Prevents GCC from optimizing away emulator pointer math
# -Wl,-z,stack-size=2097152 : Gives the Musl statically linked binary a massive 2MB stack
sed -i 's/gcc /armv7-unknown-linux-musleabihf-gcc -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static -mno-unaligned-access -fno-strict-aliasing -Wl,-z,stack-size=2097152 /g' Makefile

sed -i 's|-Isrc/|-Isrc/ -I/tmp/FBInk-master|g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile
sed -i 's|-L/usr/X11R6/lib -lX11|-L/tmp/FBInk-master -lfbink -lm|g' Makefile
sed -i 's|-lX11|-L/tmp/FBInk-master -lfbink -lm|g' Makefile
sed -i 's/strip --strip-unneeded/armv7-unknown-linux-musleabihf-strip --strip-unneeded/g' Makefile

echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
