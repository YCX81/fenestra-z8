#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
OUT=/path/to/user/.local/share/nikon-z8-qemu/live-a-v4-tests
PY=/path/to/user/.local/share/nikon-z8-qemu/integration-20261014/venv/bin/python
mkdir -p "$OUT"
bash capture/check.sh
LIVE_CODEC_TEST_OUT="$OUT/codec" bash codec/build_and_test.sh
ASAN_OPTIONS=detect_leaks=0 "$OUT/codec/transaction_test" /path/to/user/.local/share/nikon-z8-qemu/live-probe-20261104/run07/DSC_0001.JPG aus
clang -O1 -g -fsanitize=address -c codec/live_encoder.c -o "$OUT/encoder.o"
clang -O1 -g -fsanitize=address,undefined -Icapture capture/live_capture.c capture/live_stream.c codec/au_ring.c codec/live_mov.c capture/test_stream.c "$OUT/encoder.o" -o "$OUT/test_stream"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_stream" "$OUT/full-b1.mov"
clang -O1 -g -fsanitize=address,undefined codec/au_ring.c codec/test_au_ring.c -o "$OUT/test_ring"
ASAN_OPTIONS=detect_leaks=0 "$OUT/test_ring"
ffmpeg -v error -i "$OUT/full-b1.mov" -map 0:v:0 -f null -
"$PY" -B capture/test_v2_hooks.py
"$PY" -B capture/test_ticket_unicorn.py
