#!/bin/bash
# build_kindle.sh
# Automates the configuration, patching, and compilation of the Kindle PW3 port.

set -e

echo "=> Compiling Mini vMac setup tool..."
# The setup tool must be compiled natively for the host machine running the script
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM (Framebuffer API)..."
# -t larm : Target Linux ARM
# -api fb : Use the raw framebuffer API template
./setup_t -t larm -api fb > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching the generated Makefile..."
# 1. Swap out the default OS Glue file for our custom Kindle e-ink glue
# This regex catches OSGLUFBX.c, OSGLULNX.c, etc., and replaces it.
sed -i 's/OSGLU[A-Z0-9]*\.c/OSGLUKND.c/g' Makefile

# 2. Inject FBInk and evdev libraries into the Linker Flags
sed -i 's/LDFLAGS =/LDFLAGS = -lfbink /g' Makefile

echo "=> Cross-compiling the emulator..."
# Override the C compiler with the ARM hard-float toolchain
make CC=arm-linux-gnueabihf-gcc

echo "=> Build successful! Binary is ready for deployment."

