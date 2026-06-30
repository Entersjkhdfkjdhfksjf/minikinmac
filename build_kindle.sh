#!/bin/bash
set -e

# ===== VALIDATION =====
echo "=> Checking dependencies..."
command -v armv7-unknown-linux-musleabihf-gcc >/dev/null || {
    echo "ERROR: Cross-compiler not found"; exit 1
}

# ===== BUILD FBINK =====
echo "=> Building FBInk..."
if [ ! -d "/tmp/FBInk-master" ]; then
    git clone --recurse-submodules https://github.com/NiLuJe/FBInk.git /tmp/FBInk-master
fi
cd /tmp/FBInk-master
make CROSS_TC=armv7-unknown-linux-musleabihf KINDLE=1 staticlib || { echo "FBInk build failed"; exit 1; }
cd -

# ===== SETUP & PATCH =====
echo "=> Generating Mini vMac configuration..."
gcc setup/tool.c -o setup_t || { echo "Setup tool build failed"; exit 1; }
./setup_t -t larm -hres 1440 -vres 1056 -sound 0 > setup.sh
chmod +x setup.sh

rm -rf bld minivmac
./setup.sh || { echo "Setup script failed"; exit 1; }

# ===== PATCH OS GLUE =====
echo "=> Patching OS Glue layer..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# Disable problematic macros
find src cfg -type f -exec sed -i 's/M68K_USE_GLOBAL_REGS/M68K_DISABLED_GLOBAL_REGS/g' {} +
find src cfg -type f -exec sed -i 's/M68K_USE_COMPUTED_GOTO/M68K_DISABLED_COMPUTED_GOTO/g' {} +

# ===== COMPILER FLAGS =====
echo "=> Configuring compiler flags..."
CROSS_CC="armv7-unknown-linux-musleabihf-gcc"
CFLAGS="-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static -O2 -marm -mno-unaligned-access -fno-strict-aliasing -fwrapv -fno-jump-tables"
FBINK_PATH="/tmp/FBInk-master"

# Single comprehensive sed replacement
sed -i "s/^CC.*=/CC = ${CROSS_CC} ${CFLAGS}/g" Makefile
sed -i "s/^CFLAGS.*/CFLAGS = ${CFLAGS}/g" Makefile

# Add include and library paths
sed -i "s|-Isrc/|-Isrc/ -I${FBINK_PATH}/inc|g" Makefile
sed -i "s|-L/usr/X11R6/lib.*||g; s|-lX11||g" Makefile
sed -i "s/\(^LDFLAGS.*\)/\1 -L${FBINK_PATH} -lfbink -lm -lpthread/g" Makefile

# Fix strip command
sed -i "s/strip --strip-unneeded/armv7-unknown-linux-musleabihf-strip --strip-unneeded/g" Makefile

echo "=> Cross-compiling..."
make || { echo "Compilation failed"; exit 1; }

echo "=> Build successful!"
