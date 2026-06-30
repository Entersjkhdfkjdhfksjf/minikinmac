#!/bin/bash
set -e

# ===== VALIDATION =====
echo "=> Checking dependencies..."
command -v armv7-unknown-linux-musleabihf-gcc >/dev/null || {
    echo "ERROR: Cross-compiler not found in PATH"; exit 1
}

# ===== BUILD FBINK =====
echo "=> Building FBInk..."
if [ ! -d "/tmp/FBInk-master" ]; then
    git clone --recurse-submodules https://github.com/NiLuJe/FBInk.git /tmp/FBInk-master
fi
cd /tmp/FBInk-master
make CROSS_TC=armv7-unknown-linux-musleabihf KINDLE=1 staticlib || { echo "ERROR: FBInk build failed"; exit 1; }
find . -name "libfbink.a" -exec cp {} . \;
cd -

# ===== SETUP & CONFIG =====
echo "=> Generating Mini vMac configuration..."
gcc setup/tool.c -o setup_t || { echo "ERROR: Setup tool build failed"; exit 1; }
./setup_t -t larm -hres 1440 -vres 1056 -sound 0 > setup.sh
chmod +x setup.sh

echo "=> Preparing build environment..."
rm -rf bld minivmac
./setup.sh || { echo "ERROR: Setup script failed"; exit 1; }

# ===== PATCH OS GLUE =====
echo "=> Patching OS Glue layer..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

echo "=> NUKING Global Registers and Computed Gotos (Value Overwrite)..."
# CRITICAL FIX: We overwrite the macro to 0. Renaming it is not enough!
grep -rl "M68K_USE_GLOBAL_REGS" src/ cfg/ | xargs sed -i 's/.*#define.*M68K_USE_GLOBAL_REGS.*/#define M68K_USE_GLOBAL_REGS 0/g'
grep -rl "M68K_USE_COMPUTED_GOTO" src/ cfg/ | xargs sed -i 's/.*#define.*M68K_USE_COMPUTED_GOTO.*/#define M68K_USE_COMPUTED_GOTO 0/g'

# ===== COMPILER FLAGS =====
echo "=> Configuring compiler flags..."
CROSS_CC="armv7-unknown-linux-musleabihf-gcc"
CFLAGS="-mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static -O2 -marm -mno-unaligned-access -fno-strict-aliasing -fwrapv -fno-jump-tables"
FBINK_PATH="/tmp/FBInk-master"

# Remove the dangerous -Os optimization safely
sed -i 's/-Os//g' Makefile

# Replace GCC with our robust compiler and flag string
sed -i "s/gcc /${CROSS_CC} ${CFLAGS} /g" Makefile

# Add include paths (FBInk headers are in the root directory)
sed -i "s|-Isrc/|-Isrc/ -I${FBINK_PATH}|g" Makefile

# Fix linker flags and libraries (pthread and fbink must be loaded strictly at the end)
sed -i "s|-L/usr/X11R6/lib -lX11|-L${FBINK_PATH} -lfbink -lm -lpthread|g" Makefile
sed -i "s|-lX11|-L${FBINK_PATH} -lfbink -lm -lpthread|g" Makefile

# Fix strip command
sed -i "s/strip --strip-unneeded/armv7-unknown-linux-musleabihf-strip --strip-unneeded/g" Makefile

echo "=> Cross-compiling..."
make || { echo "ERROR: Compilation failed"; exit 1; }

echo "=> Build successful! Binary is ready for deployment."
