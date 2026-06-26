#!/bin/bash
# build_kindle.sh
# Automates the configuration, patching, and compilation of the Kindle PW3 port.

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM..."
# Changed -h to -hres and -v to -vres
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching the generated Makefile..."
# 1. Swap out the default X11 OS Glue file (OSGLUXWN.c) for our custom Kindle glue
sed -i 's/OSGLU[A-Z0-9]*\.c/OSGLUKND.c/g' Makefile

# 2. Strip out X11 includes from compiler flags
sed -i 's/-I\/usr\/X11R6\/include//g' Makefile

# 3. Replace the X11 linker flags (-lX11 -lXext) with our FBInk library
sed -i 's/LFLAGS = .*/LFLAGS = -lfbink/g' Makefile
sed -i 's/LDFLAGS = .*/LDFLAGS = -lfbink/g' Makefile

echo "=> Cross-compiling the emulator..."
make CC=arm-linux-gnueabihf-gcc

echo "=> Build successful! Binary is ready for deployment."

