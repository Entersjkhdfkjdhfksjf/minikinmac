#!/bin/bash
# build_kindle.sh

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

echo "=> Generating configuration for Linux ARM (X11)..."
./setup_t -t larm -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=========================================================="
echo "=> DUMPING THE MISSING INITIALIZATION LOGIC:"
echo "=========================================================="
echo "[ZapOSGLUVars]"
sed -n '/ZapOSGLUVars/,/^}/p' src/OSGLUXWN.c || true

echo -e "\n[InitOSGLU]"
sed -n '/InitOSGLU/,/^}/p' src/OSGLUXWN.c || true
echo "=========================================================="

echo "=> Diagnostic complete. Intentionally exiting."
exit 1
