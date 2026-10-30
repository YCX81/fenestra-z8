#!/bin/bash
# Baseline: unmodified Z9 C5.32 core build with the local LLVM (no flashing, emits a candidate MAIN only).
cd -- "$(dirname -- "$0")/.." || exit 1
: "${FENESTRA_Z9_MAIN:?Set FENESTRA_Z9_MAIN to the private original Z9 MAIN}"
rm -rf build/z9-baseline
mkdir -p build
python3 host/build_z9_core.py --firmware "$FENESTRA_Z9_MAIN" --output-dir build/z9-baseline 2>&1 | tail -25
echo "exit=${PIPESTATUS[0]}"
