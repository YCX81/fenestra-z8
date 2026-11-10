#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
inc=/mnt/c/path/to/user/Desktop/Nikon/_bundle-20261102/services/fenestra-z8
cc -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I. -I../live-playback -I"$inc/include" tests/test_settings.c settings.c \
  "$inc/src/native_menu.c" -o /tmp/live-playback-ui-settings-test
/tmp/live-playback-ui-settings-test
clang --target=aarch64-none-elf -Os -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
  -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror \
  -I. -I../live-playback -I"$inc/include" -c settings.c -o /tmp/live-playback-ui-settings.o
llvm-size /tmp/live-playback-ui-settings.o
