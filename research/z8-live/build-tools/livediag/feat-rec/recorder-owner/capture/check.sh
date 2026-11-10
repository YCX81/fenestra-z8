#!/bin/sh
set -eu
cd "$(dirname "$0")"
work=$(mktemp -d /tmp/live-a-capture.XXXXXX)
trap 'rm -rf "$work"' EXIT
cc -O2 -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined live_capture.c test_capture.c -o "$work/test"
# LeakSanitizer cannot work under this runner's ptrace; allocator/free counters
# and ASan/UBSan remain enabled. No firmware or QEMU is run here.
ASAN_OPTIONS=detect_leaks=0 "$work/test"
for source in live_capture.c native_capture.c; do
    clang --target=aarch64-none-elf -march=armv8-a -ffreestanding -fno-builtin -O2 -Wall -Wextra -Werror -c "$source" -o "$work/$source.o"
done
printf '%s\n' 'AArch64 freestanding compile: passed'
