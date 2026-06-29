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

echo "=> Preparing build environment..."
rm -rf bld minivmac
./setup.sh

echo "=> Patching OS Glue..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

echo "=> NUKING Global Registers and Computed Gotos in-place..."
# THE FIX: Surgically replace the '1' with a '0' directly inside the original code
find src cfg -type f \( -name "*.h" -o -name "*.c" \) -exec sed -i 's/M68K_USE_GLOBAL_REGS 1/M68K_USE_GLOBAL_REGS 0/g' {} +
find src cfg -type f \( -name "*.h" -o -name "*.c" \) -exec sed -i 's/M68K_USE_COMPUTED_GOTO 1/M68K_USE_COMPUTED_GOTO 0/g' {} +

echo "=> Patching Makefile for Musl Static Hardware Build..."
# Remove -Os
sed -i 's/-Os//g' Makefile

# Inject safe compiler flags (no jump tables, safe arm casting)
sed -i 's/gcc /armv7-unknown-linux-musleabihf-gcc -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static -O2 -marm -mno-unaligned-access -fno-strict-aliasing -fwrapv -fno-jump-tables /g' Makefile

sed -i 's|-Isrc/|-Isrc/ -I/tmp/FBInk-master|g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile

# Keep POSIX threads for stack bypass
sed -i 's|-L/usr/X11R6/lib -lX11|-L/tmp/FBInk-master -lfbink -lm -lpthread|g' Makefile
sed -i 's|-lX11|-L/tmp/FBInk-master -lfbink -lm -lpthread|g' Makefile
sed -i 's/strip --strip-unneeded/armv7-unknown-linux-musleabihf-strip --strip-unneeded/g' Makefile

echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
