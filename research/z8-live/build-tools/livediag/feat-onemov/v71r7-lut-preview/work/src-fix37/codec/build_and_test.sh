#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${LIVE_CODEC_TEST_OUT:-/path/to/user/.local/share/nikon-z8-qemu/fenestra-z8/bundle/live-a/host-tests}
SOURCE=${LIVE_CODEC_NATIVE_JPEG:-/path/to/user/.local/share/nikon-z8-qemu/live-probe-20261104/run07/DSC_0001.JPG}
mkdir -p "$OUT"
CC=${CC:-clang}
# Vendor uses deliberate null+small-offset sentinel pointers; keep vendor pristine.
# ASan applies to vendor, UBSan additionally applies to all our own code.
"$CC" -O1 -g -fsanitize=address -c codec/live_encoder.c -o "$OUT/encoder-sanitized.o"
"$CC" -O1 -g -fsanitize=address,undefined codec/live_mov.c codec/live_jpeg.c codec/live_transaction.c codec/preview_writer.c codec/transaction_test.c "$OUT/encoder-sanitized.o" -o "$OUT/transaction_test"
ASAN_OPTIONS=detect_leaks=0 "$OUT/transaction_test" "$SOURCE" | tee codec/transaction-results.txt
"$CC" -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function codec/live_encoder.c codec/live_mov.c codec/live_jpeg.c codec/host_test.c -o "$OUT/host_test"
"$OUT/host_test" "$SOURCE" "$OUT" | tee codec/memory-results.txt
ffprobe -v error -show_streams -show_format -show_packets -of json "$OUT/test.mov" > codec/ffprobe.json
ffmpeg -v error -i "$OUT/test.mov" -map 0:v:0 -f null -
exiftool -ContentIdentifier -ImageWidth -ImageHeight -MPImageType "$OUT/paired.JPG" "$OUT/test.mov" > codec/exiftool.txt
for f in live_encoder live_mov live_jpeg live_transaction native_files; do
 "$CC" --target=aarch64-none-elf -Os -std=c11 -ffreestanding -fno-builtin -D__linux__ -DNDEBUG -Icodec/freestanding -c "codec/$f.c" -o "$OUT/$f.aarch64.o"
done
llvm-size "$OUT/"*.aarch64.o | tee codec/aarch64-size.txt
llvm-nm -u "$OUT/"*.aarch64.o > codec/aarch64-undefined.txt
/path/to/user/.local/share/nikon-z8-qemu/integration-20261014/venv/bin/python -B codec/verify_artifacts.py "$SOURCE" "$OUT" | tee codec/artifact-results.json

/path/to/user/.local/share/nikon-z8-qemu/integration-20261014/venv/bin/python -B codec/native_adapter_test.py "$OUT" | tee codec/native-adapter-results.json
