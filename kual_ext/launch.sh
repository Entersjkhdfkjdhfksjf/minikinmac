#!/bin/sh
# launch.sh - KUAL execution script

# Dynamically get the directory where this extension is installed
EXT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${EXT_DIR}"

# Clear the e-ink screen before launching to prevent KUAL UI ghosting
# (Calling it twice ensures a perfectly clean slate)
eips -c
eips -c

# Stop the Kindle framework from interfering with our raw input/output
stop lab126_gui

# Launch the emulator
# We use 'exec' to replace the current shell process with the emulator
exec ./bin/minivmac

# Note: Once the emulator exits, restart the standard Kindle UI
start lab126_gui

