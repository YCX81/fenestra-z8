#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/z8-writer.XXXXXX")
cc=${CC:-clang}
flags=(-O1 -g -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror)
python3 make_inputs.py
"$cc" "${flags[@]}" -Wno-unused-function host_writer.c work/src-fix37/codec/live_mov.c work/src-fix37/codec/live_transaction.c work/src-fix37/codec/live_jpeg.c work/src-fix37/codec/preview_writer.c work/src-fix37/codec/preview_reader.c work/src-fix37/audio/live_audio_mov.c -o "$run_dir/host_writer"
"$run_dir/host_writer" input.aus input.s16 input-preview.jpg "$run_dir/sample" 0
for fault in 1 2 3; do
  "$run_dir/host_writer" input.aus input.s16 input-preview.jpg "$run_dir/fault-$fault" "$fault"
done
"$cc" "${flags[@]}" -Dlive_preview_jpeg=baseline_preview_jpeg -Dlive_preview_write=baseline_preview_write -c ../baseline-v49/work/src-fix37/codec/preview_writer.c -o "$run_dir/baseline_preview.o"
"$cc" "${flags[@]}" test_boundaries.c work/src-fix37/codec/preview_writer.c work/src-fix37/codec/preview_reader.c work/src-fix37/audio/live_audio_mov.c "$run_dir/baseline_preview.o" -o "$run_dir/test_boundaries"
"$run_dir/test_boundaries"
cp "$run_dir/sample.MOV" host-sample.MOV
python3 validate_sample.py
