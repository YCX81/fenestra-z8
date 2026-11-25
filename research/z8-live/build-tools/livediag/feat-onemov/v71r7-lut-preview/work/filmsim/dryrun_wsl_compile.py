#!/usr/bin/env python3
"""Dry-run cross-compile all 18 V69 FilmSim EL1 units on win-lab WSL (clang-21
--target=aarch64-none-elf), link standalone .filmsim at 0x409B0000..0x409D8000,
verify ELF sections & per-object FP/SIMD audit, and dry-run patch_photo_v69.py
against V68 Duplex30fps MAIN (d25307b6)."""
from __future__ import annotations

import base64
import hashlib
import io
import json
import secrets
import subprocess
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
V69 = HERE.parent.parent
V68_MAIN = Path('/Users/ycx/Nikon/build-tools/livediag/feat-onemov/v68-duplex30fps/final/main.main')
V68_SRC_FIX37 = Path('/Users/ycx/Nikon/build-tools/livediag/feat-onemov/v68-duplex30fps/work/src-fix37')
FENESTRA_INC = Path('/Users/ycx/Nikon/fenestra-z8/include')
OUT_AUDIT = V69 / 'tests/filmsim/elf_simd_audit.json'
OUT_PHOTO_DRYRUN = V69 / 'tests/filmsim/photo_hooks_dryrun.json'


def stage_tarball() -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz') as tf:
        # 1. V69 FilmSim src + build_filmsim_objs.sh
        tf.add(HERE / 'build_filmsim_objs.sh', arcname='filmsim/build_filmsim_objs.sh')
        for p in sorted((HERE / 'src').rglob('*')):
            if p.is_file():
                tf.add(p, arcname='filmsim/src/' + str(p.relative_to(HERE / 'src')))
        # 2. V68 src-fix37 headers ONLY (plus live_ui_filmsim_context_slot decl in staged header copy)
        for p in sorted(V68_SRC_FIX37.rglob('*.h')):
            rel = str(p.relative_to(V68_SRC_FIX37))
            data = p.read_bytes()
            if rel == 'ui/live_ui.h':
                text = data.decode()
                text = text.replace('#include <stdint.h>\n', '#include <stdint.h>\n#include <stdatomic.h>\n', 1)
                text = text.replace('#endif', '_Atomic(void *) *live_ui_filmsim_context_slot(void);\n#endif', 1)
                data = text.encode()
            ti = tarfile.TarInfo(name='src-fix37/' + rel)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
        # 3. fenestra-z8/include headers
        for p in sorted(FENESTRA_INC.rglob('*.h')):
            tf.add(p, arcname='fenestra-include/' + str(p.relative_to(FENESTRA_INC)))
    return buf.getvalue()


def main() -> None:
    remote_root = f'/tmp/v69-filmsim-dryrun-{secrets.token_hex(4)}'
    b64 = base64.b64encode(stage_tarball()).decode('ascii')

    remote_script = f'''set -euo pipefail
ROOT={remote_root!r}
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT"
python3 - <<'PY'
import base64, io, tarfile
raw = base64.b64decode({b64!r})
with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as tf:
    tf.extractall({remote_root!r})
PY

chmod +x "$ROOT/filmsim/build_filmsim_objs.sh"
FILMSIM_SRC="$ROOT/filmsim/src" \
SRC_FIX37_DIR="$ROOT/src-fix37" \
FENESTRA_INC_DIR="$ROOT/fenestra-include" \
OUT_DIR="$ROOT/objects" \
CC=clang-21 \
  "$ROOT/filmsim/build_filmsim_objs.sh" >&2

# Link standalone .filmsim section at 0x409B0000..0x409D8000 with stub defsyms for external V68/Fenestra hooks
cat > "$ROOT/filmsim_dryrun.ld" <<'LDS'
memcpy = 0x401c69b4;
memmove = 0x401c6a24;
memset = 0x401c69d0;
memcmp = 0x401c6658;
live_ui_filmsim_context_slot = 0x401e7000;
live_native_file_ops = 0x401ef000;
la_native_cpu2_start = 0x401e6000;
la_native_yield = 0x401e6010;

SECTIONS {{
  .filmsim 0x409b0000 : {{
    *filmsim_*.o(.text*)
    . = ALIGN(16);
    *filmsim_*.o(.rodata*)
  }}
  .data : {{ *(.data*) }}
  .bss : {{ *(.bss*) *(COMMON) }}
  ASSERT(. <= 0x409d8000, "FilmSim RX bank 0x409B0000..0x409D8000 exhausted")
  ASSERT(SIZEOF(.data) == 0, "writable .data forbidden")
  ASSERT(SIZEOF(.bss) == 0, "writable .bss forbidden")
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
LDS

ld.lld-21 -T "$ROOT/filmsim_dryrun.ld" "$ROOT"/objects/filmsim_*.o -o "$ROOT/filmsim_dryrun.elf"

python3 - <<'PY'
import json, re, subprocess
from pathlib import Path

root = Path({remote_root!r})
objs = sorted((root / 'objects').glob('filmsim_*.o'))
assert len(objs) == 14, len(objs)

# Disassembly FP/SIMD instruction audit per object (exact contract from feat-filmsim-wlm/el1/remote_build.py)
fp_re = re.compile(r'\\b(?:fadd|fsub|fmul|fdiv|fmadd|fmsub|fnmadd|fnmsub|fneg|fabs|fsqrt|fcmp|fcmpe|fccmp|fcsel|fcvt[a-z]*|scvtf|ucvtf|frint[a-z]*|fmax[a-z]*|fmin[a-z]*|frecp[a-z]*|frsq[a-z]*)\\b')
simd_re = re.compile(r'\\b(?:v\\d+\\.|[bhsdq]\\d+\\b|fmov|fadd|fsub|fmul|fdiv|scvtf|ucvtf|fcvtz|fcvt|fcmp|fmadd|fmsub|dup\\s|ushll|sshll|uaddw|usubw|uqadd|uqsub|shrn|ld[1-4]\\s*\\{{|st[1-4]\\s*\\{{|movi\\s+v)')

per_obj = {{}}
non_kernel_simd = 0
total_fp = 0
neon_allowed = {{
    'filmsim_02-z8_hdf_kernels.c.o',
    'filmsim_07-z8_zf_grain_kernels.c.o',
}}

for o in objs:
    dis = subprocess.check_output(['llvm-objdump-21', '-d', str(o)], text=True)
    fp_cnt = 0
    simd_cnt = 0
    insns = 0
    for line in dis.splitlines():
        m_ins = re.match(r'^\\s+[0-9a-f]+:\\s+([0-9a-f]{{8}})\\s+(.*)$', line)
        if m_ins:
            word_hex, rest = m_ins.groups()
            if o.name == 'filmsim_18-photo_hooks.S.o' and int(word_hex, 16) in (0x40eaafc4, 0x40eab76c, 0x40ea44d4):
                continue
            insns += 1
            if fp_re.search(rest):
                fp_cnt += 1
            if simd_re.search(rest):
                simd_cnt += 1
    # Check section sizes via llvm-readelf-21
    sh = subprocess.check_output(['llvm-readelf-21', '-SW', str(o)], text=True)
    text_b = ro_b = data_b = bss_b = 0
    for line in sh.splitlines():
        m = re.match(r'^\\s*\\[\\s*\\d+\\]\\s+(\\S+)\\s+(\\S+)\\s+[0-9a-f]+\\s+[0-9a-f]+\\s+([0-9a-f]+)\\s+[0-9a-f]+\\s+(\\S+)', line)
        if not m:
            continue
        sname, stype, ssz, sflg = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        if 'A' not in sflg:
            continue
        if 'W' in sflg:
            if stype == 'NOBITS':
                bss_b += ssz
            else:
                data_b += ssz
        elif 'X' in sflg:
            text_b += ssz
        else:
            ro_b += ssz
    total_fp += fp_cnt
    if o.name not in neon_allowed:
        non_kernel_simd += simd_cnt
    per_obj[o.name] = {{
        'text_bytes': text_b, 'rodata_bytes': ro_b, 'data_bytes': data_b, 'bss_bytes': bss_b,
        'insns': insns, 'fp_insns': fp_cnt, 'simd_insns': simd_cnt,
        'neon_allowed': o.name in neon_allowed,
    }}

# Linked ELF section & undefined symbols
elf = root / 'filmsim_dryrun.elf'
sh_elf = subprocess.check_output(['llvm-readelf-21', '-SW', str(elf)], text=True)
filmsim_va = filmsim_size = 0
writable_elf = []
for line in sh_elf.splitlines():
    m = re.match(r'^\\s*\\[\\s*\\d+\\]\\s+(\\S+)\\s+(\\S+)\\s+([0-9a-f]+)\\s+[0-9a-f]+\\s+([0-9a-f]+)\\s+[0-9a-f]+\\s+(\\S+)', line)
    if not m:
        continue
    sname, stype, sva, ssz, sflg = m.group(1), m.group(2), int(m.group(3), 16), int(m.group(4), 16), m.group(5)
    if sname == '.filmsim':
        filmsim_va, filmsim_size = sva, ssz
    if 'A' in sflg and 'W' in sflg and ssz > 0:
        writable_elf.append((sname, ssz))

undef = subprocess.check_output(['llvm-nm-21', '-u', str(elf)], text=True).strip().splitlines()
nm_def = subprocess.check_output(['llvm-nm-21', '--defined-only', str(elf)], text=True)
syms = {{}}
for line in nm_def.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[2].startswith(('z8_np_', 'filmsim_runtime_', 'z8_filmsim_', 'z8_nfsp_')):
        syms[parts[2]] = hex(int(parts[0], 16))

out = {{
    'schema': 'v69.filmsim-elf-simd-audit/v1',
    'passed': (total_fp == 0 and non_kernel_simd == 0 and not writable_elf and not undef and filmsim_va == 0x409b0000 and filmsim_va + filmsim_size <= 0x409d8000),
    'compiler': subprocess.check_output(['clang-21', '--version'], text=True).splitlines()[0],
    'units_compiled': len(objs),
    'filmsim_section': {{
        'start': hex(filmsim_va),
        'end_exclusive': hex(filmsim_va + filmsim_size),
        'size_bytes': filmsim_size,
        'bank_capacity_bytes': 0x409d8000 - 0x409b0000,
        'free_headroom_bytes': (0x409d8000 - 0x409b0000) - filmsim_size,
    }},
    'writable_sections': writable_elf,
    'undefined_symbols': undef,
    'total_fp_instructions': total_fp,
    'non_kernel_simd_instructions': non_kernel_simd,
    'neon_kernel_simd_instructions': {{k: per_obj[k]['simd_insns'] for k in sorted(neon_allowed)}},
    'objects': per_obj,
    'exported_symbols': syms,
}}
print(json.dumps(out, indent=2))
PY
'''

    res = subprocess.run(
        ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', 'win-lab', 'wsl -e bash -s'],
        input=remote_script,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if res.stderr:
        print(res.stderr, end='')
    if res.returncode != 0:
        print(res.stdout)
        raise SystemExit(res.returncode)

    audit = json.loads(res.stdout)
    assert audit['passed'], f"ELF/SIMD audit failed: {audit}"
    OUT_AUDIT.parent.mkdir(parents=True, exist_ok=True)
    OUT_AUDIT.write_text(json.dumps(audit, indent=2) + '\n')

    # Now dry-run patch_photo_v69.py against real V68 Duplex30fps MAIN using the linked 0x409B0000 symbols!
    sym_tmp = V69 / 'tests/filmsim/linked_symbols_dryrun.json'
    sym_tmp.write_text(json.dumps({'symbols': audit['exported_symbols']}, indent=2) + '\n')
    subprocess.run([
        'python3', str(HERE / 'patch_photo_v69.py'),
        '--main', str(V68_MAIN),
        '--symbols', str(sym_tmp),
        '--expected-base-sha256', hashlib.sha256(V68_MAIN.read_bytes()).hexdigest(),
        '--out-json', str(OUT_PHOTO_DRYRUN),
    ], check=True)

    print("\n=== WSL Cross-Compile & ELF/SIMD Audit Summary ===")
    print(json.dumps({
        'passed': audit['passed'],
        'units_compiled': audit['units_compiled'],
        'filmsim_section': audit['filmsim_section'],
        'writable_sections': audit['writable_sections'],
        'undefined_symbols': audit['undefined_symbols'],
        'total_fp_instructions': audit['total_fp_instructions'],
        'non_kernel_simd_instructions': audit['non_kernel_simd_instructions'],
        'neon_kernel_simd_instructions': audit['neon_kernel_simd_instructions'],
    }, indent=2))


if __name__ == '__main__':
    main()
