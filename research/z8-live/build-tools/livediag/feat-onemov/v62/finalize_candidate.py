#!/usr/bin/env python3
"""Fetch a fresh build, apply the bounded menu fix, and bind final identities."""
import base64
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile

here = Path(__file__).resolve().parent
remote = json.loads((here / 'remote-build.json').read_text())['remote_root']
build_result = json.loads((here / 'build-output.log').read_text().splitlines()[-1])
assert build_result['remote_root'] == remote
expected = build_result['main_sha256']
sha = lambda b: hashlib.sha256(b).hexdigest()

def ssh(script):
    return subprocess.check_output(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
                                    'win-lab', 'wsl -e bash -s'], input=script.encode())

files = {'pre-menu.main': 'composed/q/main.main', 'pre-menu.json': 'composed/q/main.json',
         'build.json': 'build/build.json', 'live.elf': 'build/live/payload.elf',
         'pb.elf': 'build/pb/payload.elf', 'live-ranges.json': 'build/live/changed-ranges.json',
         'pb-ranges.json': 'build/pb/changed-ranges.json', 'extra-sections.json': 'build/live/extra-sections.json'}
encoded = ssh(f'''python3 - <<'FETCH'
from pathlib import Path
import base64,tarfile,io,sys
r=Path({remote!r});files={files!r};b=io.BytesIO()
with tarfile.open(fileobj=b,mode='w:gz') as t:
 for name,path in files.items():t.add(r/path,arcname=name,recursive=False)
sys.stdout.write(base64.b64encode(b.getvalue()).decode())
FETCH
''')
build = here / 'build'
build.mkdir(exist_ok=False)
with tarfile.open(fileobj=io.BytesIO(base64.b64decode(encoded, validate=True)), mode='r:gz') as t:
    for m in t.getmembers():
        assert m.isfile() and Path(m.name).name == m.name
    t.extractall(build)
source = (build / 'pre-menu.main').read_bytes()
assert sha(source) == expected
sys.path.insert(0, str(here / 'menu'))
import patch_menu_context
menu, facts = patch_menu_context.patch(source, expected, json.loads((build / 'build.json').read_text()))
image = bytearray(menu)
at = 0x401FE060 - 0x40160000
struct.pack_into('<I', image, at, 0)
build_id = struct.unpack('<I', hashlib.sha256(image).digest()[:4])[0] | 0x01000000
struct.pack_into('<I', image, at, build_id)
digest = sha(image)
baseline = (here.parent / 'v61/final/main.main').read_bytes()
a, b = 0x40196000 - 0x40160000, 0x4019F020 - 0x40160000
assert image[a:b] == baseline[a:b]
final = here / 'final'
final.mkdir(exist_ok=False)
(final / 'main.main').write_bytes(image)
record = json.loads((build / 'pre-menu.json').read_text())
record.update(version='v62-private-candidate', sha256=digest, build_id=hex(build_id),
              physical_camera_verified=False, source_build=remote + '/build',
              scope='V61 RAW/Live playback preserved; page-qualified native Log text; finite-profile shutter sound and prepare mute with microphone ownership recovery.',
              pre_menu_sha256=expected, menu_patch_sha256=sha(menu), menu_context=facts,
              playback_bank_identical_to_v61=True)
(final / 'main.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
scripts = {name: base64.b64encode((here / 'menu' / name).read_bytes()).decode()
           for name in ['patch_menu_fallback.py', 'patch_menu_context.py']}
result = ssh(f'''python3 - <<'FINAL'
from pathlib import Path
import base64,hashlib,struct,sys,json
r=Path({remote!r});f=r/'final';f.mkdir(exist_ok=False)
for n,b in {scripts!r}.items():(f/n).write_bytes(base64.b64decode(b))
sys.path.insert(0,str(f));import patch_menu_context
b,g=patch_menu_context.patch((r/'composed/q/main.main').read_bytes(),{expected!r},json.loads((r/'build/build.json').read_text()))
b=bytearray(b);at=0x401fe060-0x40160000;struct.pack_into('<I',b,at,0)
bid=struct.unpack('<I',hashlib.sha256(b).digest()[:4])[0]|0x01000000;struct.pack_into('<I',b,at,bid)
assert hashlib.sha256(b).hexdigest()=={digest!r}
(f/'main.main').write_bytes(b)
(f/'main.json').write_text({json.dumps(record, ensure_ascii=False, indent=2) + chr(10)!r})
print('Verified final MAIN',hashlib.sha256(b).hexdigest())
FINAL
''')
print(result.decode(), end='')
print(json.dumps({'local_final': str(final), 'remote_final': remote + '/final/main.main',
                  'main_sha256': digest, 'build_id': hex(build_id),
                  'playback_bank_identical': True, 'menu_context': facts}, ensure_ascii=False))
