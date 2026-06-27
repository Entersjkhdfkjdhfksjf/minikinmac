#!/bin/bash
# build_kindle.sh

set -e

echo "=> Compiling setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating standard X11 code to reverse engineer..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=========================================================="
echo "=> 1. OFFICIAL BOOT SEQUENCE (From X11 main)"
echo "=========================================================="
grep -A 20 "int main(" src/OSGLUXWN.c || true

echo -e "\n=========================================================="
echo "=> 2. OFFICIAL HAL INTERFACE (Required OS Glue Functions)"
echo "=========================================================="
sed -i 's/gcc /arm-linux-gnueabihf-gcc /g' Makefile

# Compile only the emulator core, skipping the X11 OS Glue
make bld/MINEM68K.o bld/GLOBGLUE.o bld/M68KITAB.o bld/VIAEMDEV.o bld/IWMEMDEV.o bld/SCCEMDEV.o bld/RTCEMDEV.o bld/ROMEMDEV.o bld/SCSIEMDV.o bld/SONYEMDV.o bld/SCRNEMDV.o bld/MOUSEMDV.o bld/KBRDEMDV.o bld/SNDEMDEV.o bld/PROGMAIN.o > /dev/null 2>&1 || true

# Stitch the core together and ask the linker what functions are missing
arm-linux-gnueabihf-ld -r bld/MINEM68K.o bld/GLOBGLUE.o bld/M68KITAB.o bld/VIAEMDEV.o bld/IWMEMDEV.o bld/SCCEMDEV.o bld/RTCEMDEV.o bld/ROMEMDEV.o bld/SCSIEMDV.o bld/SONYEMDV.o bld/SCRNEMDV.o bld/MOUSEMDV.o bld/KBRDEMDV.o bld/SNDEMDEV.o bld/PROGMAIN.o -o bld/core.o || true

echo "The core emulator requires us to implement these functions:"
arm-linux-gnueabihf-nm -u bld/core.o | grep -v "__" || true
echo "=========================================================="

exit 1
