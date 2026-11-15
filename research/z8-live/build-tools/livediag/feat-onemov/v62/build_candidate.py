#!/usr/bin/env python3
"""Build a new isolated V62 source snapshot on the existing WSL lab."""
import base64
import hashlib
import io
import json
from pathlib import Path
import secrets
import subprocess
import tarfile

here = Path(__file__).resolve().parent
source = here / 'work'
root = '/path/to/user/fxfix/onemov/v62-log-shutter-' + secrets.token_hex(3)
files = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(source.rglob('*')) if p.is_file()}
data = io.BytesIO()
with tarfile.open(fileobj=data, mode='w:gz') as archive:
    for name in files:
        archive.add(source / name, arcname=name, recursive=False)
encoded = base64.b64encode(data.getvalue()).decode('ascii')
receipt = {'remote_root': root, 'files': files}
(here / 'remote-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
script = f'''set -e
python3 - <<'PY'
import base64,hashlib,io,json,tarfile
from pathlib import Path
root=Path({root!r});root.mkdir(exist_ok=False)
source=root/'source';source.mkdir()
data=base64.b64decode({encoded!r})
with tarfile.open(fileobj=io.BytesIO(data),mode='r:gz') as t:
 for m in t.getmembers():
  assert not m.name.startswith('/') and '..' not in Path(m.name).parts and m.isfile()
 t.extractall(source)
files=json.loads({json.dumps(files)!r})
assert all(hashlib.sha256((source/n).read_bytes()).hexdigest()==h for n,h in files.items())
(root/'source-input.json').write_text(json.dumps(files,indent=2)+'\\n')
print('Source snapshot verified:',len(files),'files',flush=True)
PY
/path/to/user/.local/share/nikon-z8-qemu/venv/bin/python -B /path/to/user/fxfix/onemov/build_onemov.py --v36-dir /path/to/user/v37build/v36 --source-dir {root}/source --output-dir {root}/build > {root}/build.log 2>&1
/path/to/user/.local/share/nikon-z8-qemu/venv/bin/python -B /path/to/user/fxfix/v53-root/compose53.py {root}/build {root}/composed > {root}/compose.log 2>&1
python3 - <<'PY'
import json,hashlib
from pathlib import Path
root=Path({root!r});b=json.loads((root/'build/build.json').read_text());m=json.loads((root/'composed/q/main.json').read_text())
print(json.dumps({{'remote_root':str(root),'main_sha256':m['sha256'],'build_id':m['build_id'],'sizes':{{k:v for k,v in b.items() if k.endswith('_bytes') or k.endswith('_free')}}}}))
PY
'''
result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
                         'win-lab', 'wsl -e bash -s'], input=script, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(here / 'build-output.log').write_text(result.stdout)
print(result.stdout, end='')
if result.returncode:
    print('Build failed; inspect private remote logs at', root)
raise SystemExit(result.returncode)
