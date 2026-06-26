#!/bin/bash
# build_kindle.sh
# Automates configuration, patching, compilation, and symbol dumping on failure.

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching the generated Makefile..."
# 1. Swap out OSGLUXWN for OSGLUKND
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# 2. Force the cross-compiler
sed -i 's/gcc /arm-linux-gnueabihf-gcc /g' Makefile

# 3. Inject the ARM sysroot include directory
sed -i 's|-Isrc/|-Isrc/ -I/usr/arm-linux-gnueabihf/include|g' Makefile

# 4. Strip X11 includes
sed -i 's|-I/usr/X11R6/include||g' Makefile

# 5. OBLITERATE X11 and inject FBInk + standard math library (-lm)
sed -i 's|-L/usr/X11R6/lib -lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile
sed -i 's|-lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile

# 6. Force the correct ARM strip utility
sed -i 's/strip --strip-unneeded/arm-linux-gnueabihf-strip --strip-unneeded/g' Makefile

echo "=> Cross-compiling the emulator..."
if ! make; then
    echo "=========================================================="
    echo "=> LINKER FAILED. Dumping global entry points from PROGMAIN:"
    echo "=========================================================="
    arm-linux-gnueabihf-nm bld/PROGMAIN.o | grep " T "
    echo "=========================================================="
    exit 1
fi

echo "=> Build successful! Binary is ready for deployment."
