#!/bin/bash
# build_kindle.sh - Fixed version with error checking and proper FBInk linking

set -e
set -o pipefail

trap 'echo "ERROR: Build failed at line $LINENO"; exit 1' ERR

FBINK_PATH="/tmp/FBInk-master"
CROSS_TC="armv7-unknown-linux-musleabihf"

echo "================================================================"
echo "Mini vMac for Kindle - Build Script (Fixed)"
echo "================================================================"

# ===== VALIDATE CROSS-COMPILER =====
echo "[1/8] Checking cross-compiler..."
if ! command -v "${CROSS_TC}-gcc" &> /dev/null; then
    echo "ERROR: Cross-compiler not found: ${CROSS_TC}-gcc"
    echo "Install with: sudo apt-get install gcc-arm-linux-musleabihf"
    exit 1
fi
echo "✓ Cross-compiler found: $(${CROSS_TC}-gcc --version | head -1)"

# ===== FETCH & BUILD FBINK =====
echo "[2/8] Building FBInk library..."
if [ ! -d "${FBINK_PATH}" ]; then
    echo "  Cloning FBInk repository..."
    git clone --recurse-submodules https://github.com/NiLuJe/FBInk.git "${FBINK_PATH}" || {
        echo "ERROR: Failed to clone FBInk"; exit 1
    }
fi

cd "${FBINK_PATH}"
echo "  Compiling FBInk..."
make CROSS_TC="${CROSS_TC}" KINDLE=1 staticlib -j4 || {
    echo "ERROR: FBInk compilation failed"; exit 1
}

if [ ! -f "libfbink.a" ]; then
    echo "ERROR: libfbink.a not found after build"
    find . -name "libfbink.a" -o -name "libfbink*.a"
    exit 1
fi
echo "✓ FBInk built successfully: $(ls -lh libfbink.a | awk '{print $5}')"
cd - > /dev/null

# ===== SETUP MINIVMAC =====
echo "[3/8] Building setup tool..."
if [ ! -f "setup/tool.c" ]; then
    echo "ERROR: setup/tool.c not found"
    exit 1
fi
gcc -O2 setup/tool.c -o setup_t || { echo "ERROR: Setup tool build failed"; exit 1; }
echo "✓ Setup tool compiled"

echo "[4/8] Generating Mini vMac configuration..."
./setup_t -t larm -hres 1440 -vres 1056 -sound 0 > setup.sh || {
    echo "ERROR: setup_t failed"; exit 1
}
chmod +x setup.sh
echo "✓ Configuration generated"

echo "[5/8] Running generated setup script..."
rm -rf bld minivmac
./setup.sh || { echo "ERROR: Generated setup.sh failed"; exit 1; }
echo "✓ Build directory prepared"

# ===== PATCH OS GLUE LAYER =====
echo "[6/8] Patching OS glue layer..."

# Main OS glue replacement
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile
echo "  ✓ OS glue: OSGLUXWN → OSGLUKND"

# Disable problematic 68k macros
find src cfg -type f -exec sed -i 's/M68K_USE_GLOBAL_REGS/M68K_DISABLED_GLOBAL_REGS/g' {} + 2>/dev/null
echo "  ✓ Disabled: M68K_USE_GLOBAL_REGS"

find src cfg -type f -exec sed -i 's/M68K_USE_COMPUTED_GOTO/M68K_DISABLED_COMPUTED_GOTO/g' {} + 2>/dev/null
echo "  ✓ Disabled: M68K_USE_COMPUTED_GOTO"

# ===== CONFIGURE COMPILER FLAGS =====
echo "[7/8] Configuring compiler and linker..."

# Backup original Makefile for reference
cp Makefile Makefile.backup

# Escape the FBINK_PATH for use in sed
FBINK_PATH_ESC=$(printf '%s\n' "$FBINK_PATH" | sed -e 's/[\/&]/\\&/g')

# Replace compiler invocation
sed -i "s/^CC\s*=.*/CC = ${CROSS_TC}-gcc/" Makefile
sed -i "s/^CFLAGS\s*=.*/CFLAGS = -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -static -O2 -marm -mno-unaligned-access -fno-strict-aliasing -fwrapv -fno-jump-tables -Wall/" Makefile

# Remove conflicting optimization flags
sed -i 's/ -Os / /g' Makefile
sed -i 's/ -O3 / /g' Makefile

# Fix include paths
echo "  Patching include paths..."
sed -i "s|-Isrc/|-Isrc/ -I${FBINK_PATH_ESC}|g" Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile
sed -i 's|-I/usr/include/X11||g' Makefile

# Fix library linking (replace X11 with FBInk)
echo "  Patching library linking..."
sed -i "s|-L/usr/X11R6/lib|-L${FBINK_PATH_ESC}|g" Makefile
sed -i 's|-L/usr/lib/x86_64-linux-gnu||g' Makefile
sed -i 's|-lX11|-lfbink -lm -lpthread|g' Makefile

# Fix strip command for cross-compilation
sed -i "s/strip --strip-unneeded/${CROSS_TC}-strip --strip-unneeded/g" Makefile

# Verify patches took effect
if grep -q "OSGLUKND" Makefile; then
    echo "  ✓ OS glue layer patched"
else
    echo "  WARNING: OSGLUKND not found in Makefile"
fi

if grep -q "${FBINK_PATH}" Makefile; then
    echo "  ✓ FBInk path injected"
else
    echo "  WARNING: FBInk path not found in Makefile"
fi

# Show key compiler settings
echo ""
echo "  === Compiler Settings ==="
grep "^CC" Makefile | head -1
grep "^CFLAGS" Makefile | head -1
echo ""

# ===== COMPILE =====
echo "[8/8] Cross-compiling Mini vMac for Kindle..."
echo "  Building with 4 jobs..."

if make -j4; then
    echo ""
    echo "================================================================"
    echo "✓✓✓ BUILD SUCCESSFUL ✓✓✓"
    echo "================================================================"
    echo ""
    echo "Binary location:"
    find . -name "minivmac" -type f -exec ls -lh {} \;
    echo ""
    echo "Deployment instructions:"
    echo "  1. Copy the binary to Kindle: adb push minivmac /mnt/us/..."
    echo "  2. Copy vMac.ROM: adb push vMac.ROM /mnt/us/..."
    echo "  3. SSH into Kindle and run: ./minivmac"
    echo ""
else
    echo ""
    echo "================================================================"
    echo "✗ COMPILATION FAILED"
    echo "================================================================"
    echo ""
    echo "Common issues:"
    echo "  - FBInk not built correctly: check ${FBINK_PATH}"
    echo "  - Makefile patches failed: check Makefile vs Makefile.backup"
    echo "  - Missing headers: verify -I flags point to FBInk"
    echo ""
    exit 1
fi
