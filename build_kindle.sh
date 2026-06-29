#!/bin/bash
# build_kindle.sh

set -e

echo "=> Fetching FBInk repository (with submodules)..."
if [ ! -d "/tmp/FBInk-master" ]; then
    git clone --recurse-submodules https://github.com/NiLuJe/FBInk.git /tmp/FBInk-master
fi
cd /tmp/FBInk-master
make CROSS_TC=armv7-unknown-linux-musleabihf KINDLE=1 staticlib
find . -name "libfbink.a" -exec cp {} . \;
cd -

echo "=> Compiling Mini vMac setup tool (host compiler)..."
gcc setup/tool.c -o setup_t

# -sound 0 removes the audio subsystem completely
echo "=> Generating core configuration..."
./setup_t -t larm -hres 1440 -vres 1056 -sound 0 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching Makefile for Musl Static Hardware Build..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# THE FIX: -fno-jump-tables forces safe CPU branching!
# We keep -O2 for speed, and -fwrapv ensures safe integer overflows in the emulator
sed -i 's/-Os /-O2 -marm -mno-unaligned-access -fno-strict-aliasing -fno-jump-tables -fwrapv /g' Makefile

# 4MB Stack Limit to protect against Stack Overflows
sed -i 's/gcc /armv7-unknown-linux-musleabihf-gcc -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static -Wl,-z,stack-size=4194304 /g' Makefile

sed -i 's|-Isrc/|-Isrc/ -I/tmp/FBInk-master|g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile
sed -i 's|-L/usr/X11R6/lib -lX11|-L/tmp/FBInk-master -lfbink -lm|g' Makefile
sed -i 's|-lX11|-L/tmp/FBInk-master -lfbink -lm|g' Makefile
sed -i 's/strip --strip-unneeded/armv7-unknown-linux-musleabihf-strip --strip-unneeded/g' Makefile

echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
