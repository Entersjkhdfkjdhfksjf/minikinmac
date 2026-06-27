#!/bin/sh

cd "$(dirname "$0")"

stop lab126_gui
eips -c
eips -c

./bin/minivmac > bash_crash.log 2>&1
sync

eips -c
start lab126_gui



