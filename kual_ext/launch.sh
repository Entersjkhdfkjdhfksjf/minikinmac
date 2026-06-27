#!/bin/sh

cd "$(dirname "$0")"

# 1. Gracefully stop Kindle UI
stop lab126_gui
eips -c
eips -c

# 2. Launch the statically linked emulator
# (Any terminal output/errors will be saved here so we aren't blind)
./bin/minivmac > bash_crash.log 2>&1

# 3. Sync to flash and restore
sync
eips -c
start lab126_gui


