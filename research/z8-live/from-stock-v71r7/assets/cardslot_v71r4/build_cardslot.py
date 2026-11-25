#!/usr/bin/env python3
"""Compile build/cardslot_v71r4/cardslot.c on win-lab WSL (clang-21 + ld.lld)
into cave.bin (strictly <= 0x401fef00, 3,072 B max so font_overlay 0x401fef00..0x401fff80
remains 100% zero), verify 0 FP/SIMD, 0 writable sections, 0 undefined symbols,
and fetch cave.bin + manifest.json + cave.txt back to Mac.
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
CAVE = 0x401FE300
CAVE_END = 0x401FEF00  # HARD CEILING: font_overlay starts at 0x401FEF00!
REMOTE_DIR = '/home/ycx/fxfix/onemov/v71r4-smooth-dualcard/cardslot_v71r4'

WRAPPERS = {
    'open':    (0x402DD4F8, 'str x22, [sp, #-0x30]!',     'f60f1df8'),
    'remove':  (0x402DDA4C, 'str x20, [sp, #-0x20]!',     'f40f1ef8'),
    'rename':  (0x402DD9FC, 'stp x21, x20, [sp, #-0x20]!', 'f553bea9'),
    'opendir': (0x402DDC6C, 'stp x21, x20, [sp, #-0x20]!', 'f553bea9'),
    'mkdir':   (0x402DDBDC, 'str x20, [sp, #-0x20]!',     'f40f1ef8'),
    'read':    (0x402DD6C8, 'stp x25, x24, [sp, #-0x40]!', 'f963bca9'),
    'write':   (0x402DD78C, 'sub sp, sp, #0x60',          'ff8301d1'),
}


def main() -> None:
    src_bytes = (HERE / 'cardslot.c').read_bytes()
    src_sha = hashlib.sha256(src_bytes).hexdigest()
    b64_src = base64.b64encode(src_bytes).decode('ascii')

    remote_script = f'''set -euo pipefail
ROOT={REMOTE_DIR!r}
rm -rf "$ROOT"
mkdir -p "$ROOT"

python3 - <<'PY'
import base64, hashlib, json, re, shutil, struct, subprocess
from pathlib import Path

ROOT = Path({REMOTE_DIR!r})
CAVE = {CAVE}
CAVE_END = {CAVE_END}
WRAPPERS = {json.dumps(WRAPPERS)}

src = base64.b64decode({b64_src!r})
assert hashlib.sha256(src).hexdigest() == {src_sha!r}
(ROOT / 'cardslot.c').write_bytes(src)

clang = str(Path(shutil.which('clang-21')).resolve())
ver = subprocess.check_output([clang, '--version'], text=True).splitlines()[0]
assert 'clang version 21.' in ver, ver

tramp = ['.section .text.tramp,"ax"']
defs = {{}}
for name, (entry, insn, _) in WRAPPERS.items():
    tramp += [f'.globl o_{{name}}', '.balign 4', f'o_{{name}}:', '  ' + insn, f'  b BODY_{{name}}']
    defs['BODY_' + name] = entry + 4
(ROOT / 'tramp.s').write_text('\\n'.join(tramp) + '\\n')

lds = "SECTIONS {{\\n  . = %#x;\\n  .text : {{ *(.text.tramp) *(.text*) *(.rodata*) }}\\n  /DISCARD/ : {{ *(.comment) *(.note*) *(.ARM.attributes) *(.data*) *(.bss*) }}\\n}}\\n" % CAVE
(ROOT / 'cave.ld').write_text(lds)

cflags = [
    '--target=aarch64-none-elf', '-Oz', '-ffreestanding', '-fno-builtin',
    '-fno-stack-protector', '-mgeneral-regs-only', '-mstrict-align', '-fno-pic',
    '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-mno-outline-atomics',
    '-Wall', '-Wextra', '-Werror',
]
subprocess.run([clang, *cflags, '-c', str(ROOT / 'cardslot.c'), '-o', str(ROOT / 'c.o')], check=True)
subprocess.run([clang, '--target=aarch64-none-elf', '-c', str(ROOT / 'tramp.s'), '-o', str(ROOT / 't.o')], check=True)

ld_cmd = ['ld.lld', '-T', str(ROOT / 'cave.ld'), '-e', 'hook_open', '-o', str(ROOT / 'cave.elf'),
          str(ROOT / 't.o'), str(ROOT / 'c.o')]
for k, v in defs.items():
    ld_cmd += ['--defsym', f'{{k}}={{v:#x}}']
subprocess.run(ld_cmd, check=True)

# 1. Verify no writable sections, only .text + symbol/string tables
shdr = subprocess.check_output(['llvm-readelf', '-SW', str(ROOT / 'cave.elf')], text=True)
for line in shdr.splitlines():
    m = re.match(r'^\\s*\\[\\s*\\d+\\]\\s+(\\S+)\\s+(\\S+)\\s+[0-9a-f]+\\s+[0-9a-f]+\\s+([0-9a-f]+)\\s+[0-9a-f]+\\s+(\\S+)', line)
    if m and 'A' in m.group(4):
        assert 'W' not in m.group(4), f"Writable section in cave.elf: {{line}}"
        assert m.group(1) == '.text', f"Unexpected alloc section: {{line}}"

# 2. Verify 0 undefined symbols
und = subprocess.check_output(['llvm-nm', '-u', str(ROOT / 'cave.elf')], text=True).strip()
assert not und, f"Undefined symbols: {{und}}"

# 3. Extract binary + disassembly
subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(ROOT / 'cave.elf'), str(ROOT / 'cave.bin')], check=True)
cave = (ROOT / 'cave.bin').read_bytes()
dis = subprocess.check_output(['llvm-objdump', '-d', str(ROOT / 'cave.elf')], text=True)
(ROOT / 'cave.txt').write_text(dis)

# 4. Strict size check (<= 0x401fef00 = 3,072 B!)
assert len(cave) <= CAVE_END - CAVE, f"Cave overflow: {{len(cave)}} > {{CAVE_END - CAVE}} (end={{CAVE+len(cave):#x}} > {{CAVE_END:#x}})"

# 5. Strict 0 FP/SIMD check
simd_re = re.compile(r'\\b(?:v\\d+\\.|[bhsdq]\\d+\\b|fmov|fadd|fsub|fmul|fdiv|scvtf|ucvtf|fcvtz|fcvt|fcmp|fmadd|fmsub)\\b')
bad_insns = []
for l in dis.splitlines():
    m_ins = re.match(r'^\\s*[0-9a-f]+:\\s+[0-9a-f]{{8}}\\s+(.*)$', l)
    if m_ins and simd_re.search(m_ins.group(1)):
        bad_insns.append(l.strip())
assert not bad_insns, f"FP/SIMD in cave.elf: {{bad_insns[:5]}}"

# 6. Collect symbols & hook branches
nm = subprocess.check_output(['llvm-nm', '--defined-only', str(ROOT / 'cave.elf')], text=True)
syms = {{l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}}

def branch(at, target):
    off = (target - at) >> 2
    assert -(1 << 25) <= off < (1 << 25)
    return struct.pack('<I', 0x14000000 | (off & 0x3ffffff))

hooks = {{}}
for name, (entry, insn, orig_hex) in WRAPPERS.items():
    tgt = syms['hook_' + name]
    br = branch(entry, tgt)
    hooks[name] = dict(entry=hex(entry), orig_insn=insn, orig_hex=orig_hex, branch=br.hex(), target=hex(tgt))

manifest = dict(
    schema='v71r4.cardslot/v1',
    compiler=ver,
    source_sha256={src_sha!r},
    cave_addr=hex(CAVE),
    cave_ceiling=hex(CAVE_END),
    cave_bytes=len(cave),
    cave_end=hex(CAVE + len(cave)),
    cave_free_before_font_overlay=(CAVE_END - CAVE) - len(cave),
    cave_sha256=hashlib.sha256(cave).hexdigest(),
    symbols={{k: hex(v) for k, v in sorted(syms.items())}},
    hooks=hooks,
)
(ROOT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\\n')
print(f"CARDSLOT_BUILD_OK bytes={{len(cave)}} / {{CAVE_END-CAVE}} free={{(CAVE_END-CAVE)-len(cave)}} end={{CAVE+len(cave):#x}} sha256={{manifest['cave_sha256']}}")
PY
'''
    res = subprocess.run(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 'win-lab', 'wsl -e bash -s'],
        input=remote_script, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    print(res.stdout, end='')
    if res.returncode != 0:
        raise SystemExit(res.returncode)

    fetch_script = f'''python3 - <<'PY'
import base64, io, sys, tarfile
from pathlib import Path
root = Path({REMOTE_DIR!r})
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode='w:gz') as tf:
    for fn in ('cave.bin', 'cave.elf', 'cave.txt', 'manifest.json'):
        tf.add(root / fn, arcname=fn, recursive=False)
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
        tf.extractall(HERE)
    print("Fetched cardslot artifacts into", HERE)


if __name__ == '__main__':
    main()
