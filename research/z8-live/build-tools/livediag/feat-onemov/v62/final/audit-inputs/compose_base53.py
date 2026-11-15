"""Apply a complete v52 rebuild onto exact v51d, preserving independent layers."""
import hashlib
import json
from pathlib import Path
import struct
import sys

H = Path('/path/to/user')
LOAD = 0x40160000
ID = 0x401fe060 - LOAD
OLD_RAW = '736aa971ba99a0aef3e5d155fd9db3f7c028adb09f9576761407502d2ac36e70'
OLD_FINAL = 'aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
sha = lambda b: hashlib.sha256(b).hexdigest()

def runs(a, b, step=4096):
    i = 0
    while i < len(a):
        if a[i:i+step] == b[i:i+step]:
            i += step
            continue
        end = min(len(a), i + step)
        while i < end:
            if a[i] == b[i]:
                i += 1
                continue
            first = i
            while i < len(a) and a[i] != b[i]: i += 1
            yield first, i

build, out = map(Path, sys.argv[1:3])
assert not out.exists(), out
old = (H / 'v37build/v51d/v37.main').read_bytes()
base = (H / 'fxfix/v51d/q/main.main').read_bytes()
assert sha(old) == OLD_RAW and sha(base) == OLD_FINAL
new = bytearray((build / 'v37.main').read_bytes())
raw_sha = sha(new)
assert len(new) == len(old) == len(base)
new[ID:ID+4] = old[ID:ID+4]
receipt = json.loads((build / 'build.json').read_text())
assert receipt['live_payload_bytes'] <= 0x401fd000 - 0x401e2000
assert receipt['playback_payload_bytes'] <= 0x4019f020 - 0x40196000
image = bytearray(base)
changes = list(runs(old, new))
for start, end in changes:
    assert image[start:end] == old[start:end], f'Independent v51 overlay conflict at {LOAD+start:#x}'
    image[start:end] = new[start:end]
sys.path.insert(0,'/path/to/user/fxfix/v52-root')
sys.path.insert(0,str(Path(__file__).resolve().parent))
from split_live_payload_v53 import split
from core_output_patch import apply as apply_evf, branch
(primary, extra, shutter), live_symbols, sections = split(build / 'live/payload.elf')
assert image[0x401e2000-LOAD:0x401e2000-LOAD+len(primary)] == primary
overlays = []
for address, expected, replacement, purpose in [
    (0x401fd218, bytes(len(extra)), extra, 'Live extra code'),
    (0x4019f020, bytes(len(shutter)), shutter, 'Live shutter bank in bounded playback region'),
    (0x40f6cff0, bytes.fromhex('37bf0094'), branch(0x40f6cff0,live_symbols['live_shutter_prepare_entry'],True), 'passive shutter prepare'),
    (0x40f6e174, bytes.fromhex('f8ba0094'), branch(0x40f6e174,live_symbols['live_shutter_fire_entry'],True), 'passive shutter fire')]:
    at=address-LOAD
    assert image[at:at+len(expected)] == expected, (hex(address),purpose)
    image[at:at+len(replacement)] = replacement
    overlays.append(dict(address=hex(address),size=len(replacement),purpose=purpose))
assert image[0x401fd000-LOAD:0x401fd218-LOAD] == base[0x401fd000-LOAD:0x401fd218-LOAD]
pb_symbols=json.loads((build/'pb/changed-ranges.json').read_text())['symbols']
image, evf_changes = apply_evf(image,pb_symbols)
sys.path.insert(0,str(Path('/path/to/user/fxfix/v52-root/portrait-overlay')))
from apply_portrait52_final import apply as apply_portrait
image, portrait_changes = apply_portrait(bytes(image),base)
image=bytearray(image)
struct.pack_into('<I', image, ID, 0)
build_id = struct.unpack('<I', hashlib.sha256(image).digest()[:4])[0] | 0x01000000
struct.pack_into('<I', image, ID, build_id)
out.mkdir(parents=True)
(out / 'main.main').write_bytes(image)
manifest = json.loads((H / 'fxfix/v51d/q/changed-ranges.json').read_text())
for part in ['live', 'pb']:
    manifest['symbols'].update(json.loads((build / part / 'changed-ranges.json').read_text())['symbols'])
reference = (H / 'fxfix/base-c770f340.main').read_bytes()
merged = []
for start, end in runs(reference, image):
    if merged and start - merged[-1][1] <= 4096:
        merged[-1] = merged[-1][0], end
    else:
        merged.append((start, end))
manifest['ranges'] = [dict(address=hex(LOAD+a), size=b-a, purpose='v52 aggregate overlay',
                           expected=reference[a:b].hex(), replacement=image[a:b].hex()) for a,b in merged]
manifest['output_sha256'] = sha(image)
(out / 'changed-ranges.json').write_text(json.dumps(manifest, indent=1) + '\n')
result = dict(version='v52-candidate', base_sha256=OLD_FINAL, old_rebuild_sha256=OLD_RAW,
              rebuilt_sha256=raw_sha, sha256=sha(image), build_id=hex(build_id),
              bytes=len(image), splice_runs=len(changes), physical_camera_verified=False,
              source_build=str(build), live_payload_bytes=receipt['live_payload_bytes'],
              playback_payload_bytes=receipt['playback_payload_bytes'])
result.update(shutter_bytes=len(shutter), live_extra_bytes=len(extra),extra_overlays=overlays,evf_core_changes=evf_changes)
result.update(portrait_records=len(portrait_changes),portrait_changed_bytes=sum(
    x!=y for e in portrait_changes for x,y in zip(bytes.fromhex(e['expected']),bytes.fromhex(e['replacement']))))
(out / 'portrait-ranges.json').write_text(json.dumps(portrait_changes,indent=2)+'\n')
(out / 'main.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
