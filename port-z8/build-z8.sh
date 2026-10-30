#!/bin/bash
# Compile the Z8 C3.11 core candidate (emulation only, never flashed).
cd /mnt/c/Users/ycx/Desktop/Nikon/third-party-20261029/fenestra-z8 || exit 1
rm -rf build/z8-core
python3 host/build_z8_core.py --firmware /mnt/c/Users/ycx/Desktop/Nikon/z8-integration-20261014/.local/firmware/z8-c311-main.bin --output-dir build/z8-core 2>&1 | tail -40
echo "exit=${PIPESTATUS[0]}"
