"""Read-only media/identity audit of a completed private QEMU capture directory."""
from pathlib import Path
import array
import hashlib
import json
import math
import struct
import subprocess
import sys

p = Path(sys.argv[1])
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
result = json.loads((p / 'result.json').read_text())
mov = next((p / 'files/LIVE').rglob('*.MOV'))
jpg = next((p / 'files/DCIM').rglob('*.JPG'))
man = next((p / 'files/LIVEDATA').rglob('*.MAN'))
probe = subprocess.check_output(['ffprobe', '-v', 'error', '-show_format', '-show_streams', '-of', 'json', str(mov)])
(p / 'mov-ffprobe.json').write_bytes(probe)
decode = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(mov), '-map', '0:v:0', '-f', 'null', '-'], capture_output=True)
assert decode.returncode == 0, decode.stderr
(p / 'video-decode.log').write_bytes(decode.stderr)
audio = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(mov), '-map', '0:a:0', '-c:a', 'pcm_s16le', '-f', 's16le', '-'], capture_output=True)
assert audio.returncode == 0, audio.stderr
(p / 'audio-decode.log').write_bytes(audio.stderr)
(p / 'mov-audio.s16le').write_bytes(audio.stdout)
a = array.array('h', audio.stdout)
assert sys.byteorder == 'little' and len(a) % 2 == 0
frames = len(a) // 2
nz = [i for i in range(frames) if a[2*i] or a[2*i+1]]
zeros, begin = [], None
for i in range(frames + 1):
    zero = i < frames and not (a[2*i] or a[2*i+1])
    if zero and begin is None:
        begin = i
    elif not zero and begin is not None:
        if i - begin >= 480:
            zeros.append(dict(start_frame=begin, end_frame=i, seconds=(i-begin)/48000))
        begin = None

# LVJB points to exactly the native LPCM bytes, not a second synthesized track.
data = mov.read_bytes()
marker = data.rfind(b'LVJB')
assert marker > 0
offset, lv_frames, rate = struct.unpack_from('<III', data, marker + 4)
assert lv_frames == frames and rate == 48000
pcm24 = data[offset:offset + frames * 6]
assert len(pcm24) == frames * 6 and not any(pcm24[::3])
pcm16 = bytes(v for i, v in enumerate(pcm24) if i % 3)
assert pcm16 == audio.stdout
manifest = dict(line.split('=', 1) for line in man.read_text().splitlines())
assert int(manifest['size']) == jpg.stat().st_size
assert manifest['native'].endswith(jpg.name)
uuid = manifest['uuid'].encode()
assert uuid in data and uuid in (p / 'files/LIVE' / jpg.parent.name / jpg.name).read_bytes()
assert b'com.apple.quicktime.still-image-time' in data
summary = dict(frames=frames, seconds=frames/48000, nonzero_samples=sum(x != 0 for x in a),
    leading_zero_frames=min(nz), trailing_zero_frames=frames-1-max(nz), zero_runs_10ms=zeros,
    peak=max(abs(x) for x in a), rms=math.sqrt(sum(x*x for x in a)/len(a)),
    sha256=hashlib.sha256(audio.stdout).hexdigest(), lvjb_same_track=True,
    manifest=manifest, apple_pair_uuid=True, apple_still_time_key=True,
    video_decode_errors=decode.stderr.decode(), audio_decode_errors=audio.stderr.decode())
(p / 'pcm-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
cmd = result['command']
kernel, qemu = Path(cmd[cmd.index('-kernel')+1]), Path(cmd[0])
identity = dict(kernel=str(kernel), kernel_sha256=sha(kernel), qemu=str(qemu), qemu_sha256=sha(qemu),
    prepared_stock_label=result['firmware_sha256'], live_a=result['live_a'],
    label_warning='Original firmware/readiness labels may describe prepared stock. Use executed command kernel, launch manifest and this audit; original result is unchanged.',
    driver_error=result.get('error'), checks=dict(total=len(result['checks']), failed=[x for x in result['checks'] if not x['passed']]),
    media=[dict(path=str(f.relative_to(p)), bytes=f.stat().st_size, sha256=sha(f)) for f in (jpg, mov, man)],
    final_runtime=result['live_checkpoints'][-1]['runtime'])
(p / 'identity-and-capture-summary.json').write_text(json.dumps(identity, indent=2) + '\n')
print(json.dumps(dict(pcm=summary, media=identity['media'], error=identity['driver_error']), indent=2))
