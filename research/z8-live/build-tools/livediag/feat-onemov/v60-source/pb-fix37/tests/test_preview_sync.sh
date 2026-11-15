#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cc -std=c11 -Wall -Wextra -Werror -c preview_writer.c -o /tmp/live-playback-ui-preview-writer.o
cc -std=c11 -Wall -Wextra -Werror -c sync.c -o /tmp/live-playback-ui-sync.o
cc -std=c11 -Wall -Wextra -Werror -c native_sync.c -o /tmp/live-playback-ui-native-sync.o
cc -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -g -I. tests/test_preview_sync.c preview_writer.c sync.c ../live-playback/preview.c -o /tmp/live-playback-ui-preview-sync-test
# LeakSanitizer cannot run under this workspace's ptrace supervisor.
ASAN_OPTIONS=detect_leaks=0 /tmp/live-playback-ui-preview-sync-test
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I. tests/test_preview_sync_native.c native_sync.c sync.c -o /tmp/live-playback-ui-preview-sync-native-test
ASAN_OPTIONS=detect_leaks=0 /tmp/live-playback-ui-preview-sync-native-test
