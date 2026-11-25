#!/usr/bin/env python3
"""Stage 3 — Apply FontCJK-ZhUI LAST on top of Stage 2 (PortraitPB) on win-lab WSL
with clang + ld.lld, dynamically resolving ORIG_TEXT_VA from 0x4032E130 (cross-checked
against stage1-ranges.json), stamping final Bootguard Build ID at 0x401FE060, and
running audit_v68.py (9/9 preserved banks, 0 unexplained bytes, orig_text_call_verified).
"""
from __future__ import annotations

import base64
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
V71 = HERE.parent
FONT_ZH = V71 / 'work/font-zh'
PORTRAIT_PB = V71.parent / 'v63-portrait-pb'
FINAL = V71 / 'final'
REMOTE_STAGE1 = '/home/ycx/fxfix/onemov/v71r7-lut-preview/stage1/out'
REMOTE_ROOT = '/home/ycx/fxfix/onemov/v71r7-lut-preview/stage3'


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pack_stage3_inputs() -> tuple[bytes, dict[str, str]]:
    files: dict[str, str] = {}
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tf:
        for rel in ('patch_v68.py', 'audit_v68.py', 'build_cjk_hif.py', 'v68_patch_code.c',
                    'fonts/cjk_ext_1.hif', 'fonts/cjk_ext_2.hif'):
            p = FONT_ZH / rel
            arc = 'font-zh/' + rel
            files[arc] = sha(p.read_bytes())
            tf.add(p, arcname=arc, recursive=False)
        p_ppb = PORTRAIT_PB / 'patch_portrait_pb.py'
        files['patch_portrait_pb.py'] = sha(p_ppb.read_bytes())
        tf.add(p_ppb, arcname='patch_portrait_pb.py', recursive=False)
    return buf.getvalue(), files


def main() -> None:
    stage1_meta = json.loads((FINAL / 'stage1-build.json').read_text())
    stage2_meta = json.loads((FINAL / 'stage2-portrait.json').read_text())
    stage1_sha = stage1_meta['stage1_sha256']
    stage2_sha = stage2_meta['final_main_sha256']
    assert sha((FINAL / 'stage2.main').read_bytes()) == stage2_sha

    raw_tar, files = pack_stage3_inputs()
    b64_tar = base64.b64encode(raw_tar).decode('ascii')

    remote_script = f'''set -euo pipefail
ROOT={REMOTE_ROOT!r}
rm -rf "$ROOT"
mkdir -p "$ROOT/src" "$ROOT/out"

python3 - <<'PY'
import base64, hashlib, io, json, tarfile
from pathlib import Path
root = Path({REMOTE_ROOT!r})
src = root / 'src'
data = base64.b64decode({b64_tar!r})
with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tf:
    for m in tf.getmembers():
        assert not m.name.startswith('/') and '..' not in Path(m.name).parts and m.isfile()
    tf.extractall(src)
expected = json.loads({json.dumps(files)!r})
for rel, h in expected.items():
    got = hashlib.sha256((src / rel).read_bytes()).hexdigest()
    assert got == h, f"Upload hash mismatch: {{rel}} {{got}} != {{h}}"
print(f"Verified {{len(expected)}} uploaded Stage 3 files", flush=True)
PY

# Reproduce exact Stage 2 PortraitPB on WSL from Stage 1 (avoids re-uploading 81.8 MB) and assert exact SHA-256!
python3 "$ROOT/src/patch_portrait_pb.py" \
  --input {REMOTE_STAGE1}/stage1.main \
  --expected-sha256 {stage1_sha} \
  --build-manifest {REMOTE_STAGE1}/stage1-build.json \
  --output "$ROOT/out/stage2.main" \
  --meta "$ROOT/out/stage2-portrait.json" \
  --version v71r7-lut-preview-stage2-portraitpb > /dev/null

python3 -c "import hashlib, pathlib; s=hashlib.sha256(pathlib.Path('$ROOT/out/stage2.main').read_bytes()).hexdigest(); assert s == '{stage2_sha}', (s, '{stage2_sha}'); print('Verified WSL Stage 2 SHA-256:', s)"

clang --version | head -n 1

# Run Stage 3 FontCJK-ZhUI patch_v68.py LAST
python3 "$ROOT/src/font-zh/patch_v68.py" \
  --base-main "$ROOT/out/stage2.main" \
  --expected-base-sha256 {stage2_sha} \
  --build-manifest {REMOTE_STAGE1}/stage1-ranges.json \
  --cjk1 "$ROOT/src/font-zh/fonts/cjk_ext_1.hif" \
  --cjk2 "$ROOT/src/font-zh/fonts/cjk_ext_2.hif" \
  --c-src "$ROOT/src/font-zh/v68_patch_code.c" \
  --version v71r7-lut-preview \
  --out-main "$ROOT/out/main.main" \
  --out-json "$ROOT/out/main.json"

# Run Stage 3 independent binary audit on WSL
python3 "$ROOT/src/font-zh/audit_v68.py" \
  --base-main "$ROOT/out/stage2.main" \
  --expected-base-sha256 {stage2_sha} \
  --v68-main "$ROOT/out/main.main" \
  --v68-json "$ROOT/out/main.json" \
  --cjk1 "$ROOT/src/font-zh/fonts/cjk_ext_1.hif" \
  --cjk2 "$ROOT/src/font-zh/fonts/cjk_ext_2.hif" \
  --out-json "$ROOT/out/stage3-fontzh-audit.json"
'''

    res = subprocess.run(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 'win-lab', 'wsl -e bash -s'],
        input=remote_script, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    print(res.stdout, end='')
    if res.returncode != 0:
        raise SystemExit(res.returncode)

    # Fetch final/main.main, final/main.json, final/stage3-fontzh-audit.json back to Mac
    fetch_script = f'''python3 - <<'PY'
import base64, io, sys, tarfile
from pathlib import Path
out = Path({REMOTE_ROOT!r}) / 'out'
buf = io.BytesIO()
files = {{
    'final/main.main': out / 'main.main',
    'final/main.json': out / 'main.json',
    'final/stage3-fontzh-audit.json': out / 'stage3-fontzh-audit.json',
}}
with tarfile.open(fileobj=buf, mode='w:gz') as tf:
    for arc, p in files.items():
        tf.add(p, arcname=arc, recursive=False)
sys.stdout.write(base64.b64encode(buf.getvalue()).decode('ascii'))
PY
'''
    encoded = subprocess.check_output(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 'win-lab', 'wsl -e bash -s'],
        input=fetch_script.encode(),
    )
    with tarfile.open(fileobj=io.BytesIO(base64.b64decode(encoded, validate=True)), mode='r:gz') as tf:
        for m in tf.getmembers():
            assert m.isfile() and not m.name.startswith('/') and '..' not in Path(m.name).parts
        tf.extractall(V71)

    meta = json.loads((FINAL / 'main.json').read_text())
    assert sha((FINAL / 'main.main').read_bytes()) == meta['main_sha256']

    # Re-run audit_v68.py independently on Mac against local stage2.main -> final/main.main!
    subprocess.run([
        'python3', str(FONT_ZH / 'audit_v68.py'),
        '--base-main', str(FINAL / 'stage2.main'),
        '--expected-base-sha256', stage2_sha,
        '--v68-main', str(FINAL / 'main.main'),
        '--v68-json', str(FINAL / 'main.json'),
        '--out-json', str(FINAL / 'stage3-fontzh-audit.json'),
    ], check=True)
    print("Stage 3 complete and independently audited on Mac:", FINAL / 'main.main', meta['main_sha256'])


if __name__ == '__main__':
    main()
