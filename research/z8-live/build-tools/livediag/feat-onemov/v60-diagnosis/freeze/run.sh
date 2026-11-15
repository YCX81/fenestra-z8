#!/bin/sh
set -eu
fixture_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
audit_tmp=$(mktemp -d "${TMPDIR:-/tmp}/v60-freeze-replay.XXXXXX")
trap 'rm -rf "$audit_tmp"' EXIT HUP INT TERM
"${CC:-clang}" -std=c11 -O1 -g -fsanitize=address,undefined "$fixture_dir/test.c" -o "$audit_tmp/test"
"$audit_tmp/test"
