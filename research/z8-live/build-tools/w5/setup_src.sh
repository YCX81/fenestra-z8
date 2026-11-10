#!/bin/bash
# Build tree for the Wave5 Live payload: audio tree + Wave5 encoder in place of minih264.
set -euo pipefail
L=$HOME/.local/share/nikon-z8-qemu/live-audio
rm -rf "$L/src-w5"; cp -a "$L/src-audio" "$L/src-w5"
cp "$L/w5in/live_w5enc.c" "$L/w5in/live_h264_idr.c" "$L/w5in/live_h264_idr.h" "$L/src-w5/codec/"
python3 - "$L/src-w5/codec/live_codec.h" <<'P'
import sys; p=sys.argv[1]; t=open(p).read()
old="typedef struct { void *persist, *scratch; unsigned width,height,frames; uint64_t last_pts; } live_encoder;"
new="typedef struct { void *persist, *scratch; unsigned width,height,frames; uint64_t last_pts; struct live_w5 *hw; } live_encoder;"
assert old in t; open(p,'w').write(t.replace(old,new))
P
sed -e "s#'codec/live_encoder.c', #'codec/live_w5enc.c', 'codec/live_h264_idr.c', #" \
    -e "s#LIMIT = 0x40200000#LIMIT = 0x401fff80#" \
    "$L/relive_audio.py" > "$L/relive_w5.py"
python3 "$L/w5in/coexist_patch.py" "$L/src-w5" "$L/relive_w5.py"
python3 "$L/w5in/flat_patch.py" "$L/src-w5"
grep -n "live_w5enc\|LIMIT =\|W5_SITE" "$L/relive_w5.py"
