#!/bin/bash
# Compile the Z8 C3.11 core candidate (emulation only, never flashed).
cd -- "$(dirname -- "$0")/.." || exit 1
: "${FENESTRA_Z8_MAIN:?Set FENESTRA_Z8_MAIN to the private original Z8 MAIN}"
rm -rf build/z8-core
python3 host/build_z8_core.py --firmware "$FENESTRA_Z8_MAIN" --output-dir build/z8-core 2>&1 | tail -40
echo "exit=${PIPESTATUS[0]}"
