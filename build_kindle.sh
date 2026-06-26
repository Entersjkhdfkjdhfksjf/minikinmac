#!/bin/bash
# build_kindle.sh
# Automates configuration, Framebuffer API generation, and FBInk thread injection.

set -e

echo "=> Compiling Mini vMac setup tool..."
gcc setup/tool.c -o setup_t

# 1. Pivot to the Native Linux Framebuffer API (-api fb)
echo "=> Generating configuration for Linux Framebuffer..."
./setup_t -t larm -api fb -hres 1440 -vres 1056 > setup.sh
chmod +x setup.sh
./setup.sh

echo "=> Patching Makefile for cross-compilation..."
sed -i 's/gcc /arm-linux-gnueabihf-gcc /g' Makefile
sed -i 's/strip --strip-unneeded/arm-linux-gnueabihf-strip --strip-unneeded/g' Makefile

# Safely append our FBInk and Pthread libraries to the linker command
sed -i 's/PROGMAIN.o/PROGMAIN.o -L\/usr\/arm-linux-gnueabihf\/lib -lfbink -lm -lpthread -ldl/g' Makefile

echo "=> Injecting FBInk background thread into OSGLULFB.c..."
# Create a header containing our e-ink refresh thread
cat << 'EOF' > fbink_thread.h
#include <pthread.h>
#include "fbink.h"
#include <unistd.h>
static void* eink_refresh_thread(void* arg) {
    int fbfd = fbink_open();
    FBInkConfig fb_cfg = {0};
    fbink_init(fbfd, &fb_cfg);
    fb_cfg.is_flashing = false;
    fb_cfg.wfm_mode = WFM_A2;
    while(1) {
        // Force the kernel framebuffer to the physical e-ink screen at 20 FPS
        fbink_refresh(fbfd, 0, 0, 0, 0, &fb_cfg);
        usleep(50000); 
    }
    return NULL;
}
EOF

# Inject the thread function at the very top of the generated file
sed -i '1r fbink_thread.h' src/OSGLULFB.c

# Inject the thread launch command right as the emulator boots
sed -i '/int main(/a \    pthread_t thread_id;\n    pthread_create(\&thread_id, NULL, eink_refresh_thread, NULL);' src/OSGLULFB.c

echo "=> Cross-compiling the emulator..."
make

echo "=> Build successful! Binary is ready for deployment."
