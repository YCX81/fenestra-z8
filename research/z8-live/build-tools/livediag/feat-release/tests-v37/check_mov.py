#!/usr/bin/env python3
"""Check a v37 Live MOV from a card: ordinary players see a normal movie, and the
trailing free atom holds the 45 in-camera preview pictures.
usage: check_mov.py FILE.MOV [OUT_DIR]   (needs ffprobe/ffmpeg)"""
import json, struct, subprocess, sys
from pathlib import Path

path = Path(sys.argv[1])
out = Path(sys.argv[2]) if len(sys.argv) > 2 else None
data = path.read_bytes()
# top-level atoms
atoms, at = [], 0
while at + 8 <= len(data):
    size, kind = struct.unpack_from('>I4s', data, at)
    assert 8 <= size <= len(data) - at, ('bad atom', at, size)
    atoms.append((kind.decode('latin1'), at, size)); at += size
assert at == len(data), 'trailing bytes outside atoms'
assert [a[0] for a in atoms] == ['ftyp', 'mdat', 'moov', 'free'], atoms
total, tag = struct.unpack_from('<I4s', data, len(data) - 8)
assert tag == b'LVJT' and total == atoms[3][2], (tag, total, atoms[3])
base = atoms[3][1]
magic, version, head, w, h, frames, frame_us, zero = struct.unpack_from('<4sHHHHIII', data, base + 8)
assert (magic, version, w, h, frames, frame_us, zero) == (b'LVJ1', 1, 1024, 682, 45, 66667, 0) and head == 24 + 4 * frames
sizes = struct.unpack_from('<%dI' % frames, data, base + 32)
at = base + 8 + head
distinct = set()
if out: out.mkdir(parents=True, exist_ok=True)
for i, n in enumerate(sizes):
    jpg = data[at:at + n]
    assert jpg[:2] == b'\xff\xd8' and jpg[-2:] == b'\xff\xd9' and n <= 192 * 1024, (i, n)
    distinct.add(hash(jpg))
    if out: (out / ('f%02d.jpg' % i)).write_bytes(jpg)
    at += n
assert at <= len(data) - 8
probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(path)]))
video = next(s for s in probe['streams'] if s['codec_type'] == 'video')
decoded = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-f', 'null', '-'], capture_output=True, text=True)
assert decoded.returncode == 0 and not decoded.stderr.strip(), decoded.stderr[:300]
if out:
    r = subprocess.run(['ffmpeg', '-v', 'error', '-y', '-framerate', '15', '-i', str(out / 'f%02d.jpg'), '-f', 'null', '-'],
                       capture_output=True, text=True)
    assert r.returncode == 0 and not r.stderr.strip(), r.stderr[:300]
print(json.dumps(dict(file=str(path), bytes=len(data), movie_bytes=base, preview_bytes=total, preview_frames=frames,
                      distinct_preview_frames=len(distinct), avg_jpeg=sum(sizes) // frames, max_jpeg=max(sizes),
                      video=dict(codec=video['codec_name'], width=video['width'], height=video['height'],
                                 frames=video.get('nb_frames'), duration=video.get('duration')),
                      streams=[s['codec_type'] for s in probe['streams']], ffmpeg_full_decode='ok')))
