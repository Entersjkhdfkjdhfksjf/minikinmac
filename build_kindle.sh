#!/bin/bash
# build_kindle.sh
# Automates the configuration, patching, and compilation of the Kindle PW3 port.

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching the generated Makefile..."
# 1. Swap OSGLUXWN for OSGLUKND everywhere (fixes both the .c source and .o output paths)
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile

# 2. Force the cross-compiler by replacing 'gcc ' directly on the compilation lines
sed -i 's/gcc /arm-linux-gnueabihf-gcc /g' Makefile

# 3. Inject the ARM sysroot include directory into the compilation flags
# (Mini vMac includes '-Isrc/' on every line, so we append to that)
sed -i 's|-Isrc/|-Isrc/ -I/usr/arm-linux-gnueabihf/include|g' Makefile

# 4. Strip X11 includes
sed -i 's|-I/usr/X11R6/include||g' Makefile

# 5. Inject FBInk library and path into the linker flags
sed -i 's|LFLAGS = .*|LFLAGS = -L/usr/arm-linux-gnueabihf/lib -lfbink|g' Makefile
sed -i 's|LDFLAGS = .*|LDFLAGS = -L/usr/arm-linux-gnueabihf/lib -lfbink|g' Makefile

echo "=> Cross-compiling the emulator..."
# We no longer need to pass CC= here, as the Makefile is now hardcoded for ARM
make

echo "=> Build successful! Binary is ready for deployment."
