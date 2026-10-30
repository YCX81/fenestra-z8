#!/bin/bash
# Baseline: unmodified Z9 C5.32 core build with the local LLVM (no flashing, emits a candidate MAIN only).
cd /mnt/c/Users/ycx/Desktop/Nikon/third-party-20261029/fenestra-z8 || exit 1
rm -rf build/z9-baseline
mkdir -p build
python3 host/build_z9_core.py --firmware /mnt/c/Users/ycx/Desktop/Nikon/z8-mcu-audit-20261019/.local/z9modules/eg1990_053200.bi --output-dir build/z9-baseline 2>&1 | tail -25
echo "exit=${PIPESTATUS[0]}"
