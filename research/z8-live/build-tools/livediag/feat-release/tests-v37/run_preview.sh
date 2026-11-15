#!/usr/bin/env bash
# Host checks for the v37 embedded preview. Needs clang and ffmpeg; no camera, USB or QEMU.
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=0
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-/tmp/v37-preview}
rm -rf "$out"; mkdir -p "$out/jpg-a" "$out/jpg-b"
clang -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror \
  "$here/test_preview.c" "$here/../src-fix37/codec/preview_writer.c" "$here/../src-fix37/codec/preview_reader.c" -lm -o "$out/test_preview"
# a: synthetic pattern with motion; b: noise-heavy worst case for the size ladder
ffmpeg -v error -y -f lavfi -i "testsrc2=size=1024x682:rate=15" -frames:v 45 -pix_fmt yuv420p -f rawvideo "$out/a.yuv"
ffmpeg -v error -y -f lavfi -i "mandelbrot=size=1024x682:rate=15,noise=alls=60:allf=t" -frames:v 45 -pix_fmt yuv420p -f rawvideo "$out/b.yuv"
for k in a b; do
  "$out/test_preview" "$out/$k.yuv" "$out/$k.atom" "$out/$k.dec.yuv" 4096 -1 "$out/jpg-$k"
  # every picture must be a standard JPEG: an independent decoder agrees with ours
  ffmpeg -v error -y -f image2 -framerate 15 -i "$out/jpg-$k/f%02d.jpg" -pix_fmt yuvj420p -f rawvideo "$out/$k.ff.yuv"
  python3 - "$out/$k.dec.yuv" "$out/$k.ff.yuv" <<'PY'
import sys, math
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
assert len(a) == len(b) == 45 * 1024 * 682 * 3 // 2, (len(a), len(b))
n = 1024 * 682  # luma of every frame: chroma upsampling/rounding conventions differ between decoders
worst = 99.0; peak = 0
for f in range(45):
    o = f * n * 3 // 2
    x = a[o:o + n]; y = b[o:o + n]
    e = sum((p - q) * (p - q) for p, q in zip(x[::7], y[::7]))
    peak = max(peak, max(abs(p - q) for p, q in zip(x[::7], y[::7])))
    worst = min(worst, 99.0 if not e else 10 * math.log10(255 * 255 * len(x[::7]) / e))
print(f'ffmpeg-vs-ours luma psnr_min={worst:.2f} max_abs_diff={peak}')
assert worst > 45 and peak <= 4
PY
done
# a failed card write at any point must surface as an error
for n in 0 1 5 40; do "$out/test_preview" "$out/a.yuv" "$out/x.atom" "$out/x.yuv" 4096 $n; done
echo PASS
