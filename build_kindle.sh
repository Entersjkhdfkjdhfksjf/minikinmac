#!/bin/bash
# build_kindle.sh
# Automates configuration, patching, dynamic entry-point discovery, and compilation.

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching the generated Makefile..."
sed -i 's/OSGLUXWN/OSGLUKND/g' Makefile
sed -i 's/gcc /arm-linux-gnueabihf-gcc /g' Makefile
sed -i 's|-Isrc/|-Isrc/ -I/usr/arm-linux-gnueabihf/include|g' Makefile
sed -i 's|-I/usr/X11R6/include||g' Makefile
sed -i 's|-L/usr/X11R6/lib -lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile
sed -i 's|-lX11|-L/usr/arm-linux-gnueabihf/lib -lfbink -lm -ldl|g' Makefile

echo "=> Compiling all objects..."
# We run make and allow it to fail the final link step because OSGLUKND.c 
# currently uses a placeholder entry point.
make || true

echo "=> Auto-discovering the true Mini vMac entry point..."
# The true entry function is called inside main() in the original OSGLUXWN.c
# We extract the function call that doesn't contain "OSGlue", "return", or "if"
REAL_ENTRY=$(grep -A 15 "int main" src/OSGLUXWN.c | grep -v "OSGlue" | grep -v "return" | grep -v "if" | grep -oE "[a-zA-Z0-9_]+\s*\(" | tr -d ' (' | head -n 1)

if [ -z "$REAL_ENTRY" ]; then
    echo "Failed to auto-discover entry point. Using fallback."
    REAL_ENTRY="vMacMain"
fi

echo "=> True Entry Point Discovered: $REAL_ENTRY"

echo "=> Patching OSGLUKND.c with $REAL_ENTRY..."
sed -i "s/MainEventLoop/$REAL_ENTRY/g" src/OSGLUKND.c

echo "=> Recompiling OSGLUKND.c..."
arm-linux-gnueabihf-gcc "src/OSGLUKND.c" -o "bld/OSGLUKND.o" -c -Wall -Wmissing-prototypes -Wno-uninitialized -Wundef -Wstrict-prototypes -Icfg/ -Isrc/ -I/usr/arm-linux-gnueabihf/include -Os

echo "=> Final Linking..."
make

echo "=> Build successful! Binary is ready for deployment."

