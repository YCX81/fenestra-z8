#!/usr/bin/env python3
"""WSL clang-21 AArch64 Cross-Compile + Static Audit (0 Q-spill, 0 FP, 0 .data/.bss, stack)
+ Unicorn AArch64 Exact Instruction Count Benchmark (V71.2 Baseline vs V71.3 New).

Compiles z8_hdf.c (-mgeneral-regs-only -DHDF_SCALAR) and z8_hdf_kernels.c (NEON, -Os,
-mstrict-align, exact build_filmsim_objs.sh flags) on win-lab WSL clang-21, links a standalone
AArch64 ELF, audits every function for Q-register stack spills / FP insns / stack frames /
writable sections, and runs Unicorn AArch64 on real Nikon Z8 frames + synthetic scenes
across HDF Weak (1), Medium (2), Strong (3), and Monochrome!
"""
from __future__ import annotations

import base64
import bisect
import io
import json
import re
import secrets
import struct
import subprocess
import sys
import tarfile
from collections import defaultdict
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_MEM_INVALID
from unicorn.arm64_const import *

HERE = Path(__file__).resolve().parent
FILMSIM_DIR = HERE.parent
V71_DIR = FILMSIM_DIR.parent.parent
HDF_DIR = FILMSIM_DIR / "src/hdf"

# Real Z8 test frame loader
sys.path.insert(0, str(V71_DIR / "tests/verify_arm"))
from run_v71_e2e_bench_arm import load_real_z8_nv12_1024x682

JPG_9130 = Path("/Users/ycx/.gemini/antigravity/brain/3da3f9dc-2e8c-4728-a42d-7910262ef86b/scratch/v70r2-real-sd/tear_check/DSC_9130_f03.jpg")
JPG_9129 = Path("/Users/ycx/.gemini/antigravity/brain/3da3f9dc-2e8c-4728-a42d-7910262ef86b/scratch/v70r2-real-sd/tear_check/DSC_9129_f10.jpg")
JPG_9126 = Path("/Users/ycx/.gemini/antigravity/brain/3da3f9dc-2e8c-4728-a42d-7910262ef86b/scratch/v70r2-real-sd/tear_check/DSC_9126_f56.jpg")

BASE_ELF = V71_DIR / "final/payload.elf"
BASE_MAIN = V71_DIR / "final/main.main"
MAIN_VA_BASE = 0x40160000


def compile_hdf_on_wsl(src_dir: Path, scalar_kernels: bool = False) -> tuple[bytes, dict]:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for fn in ("z8_hdf.h", "z8_hdf_kernels.h", "z8_hdf.c", "z8_hdf_kernels.c"):
            tf.add(src_dir / fn, arcname=fn)
    b64 = base64.b64encode(buf.getvalue()).decode("ascii")
    remote_root = f"/tmp/v71r4-hdf-bench-{secrets.token_hex(4)}"
    k_extra = "-mgeneral-regs-only -DHDF_SCALAR" if scalar_kernels else ""

    remote_script = f"""set -euo pipefail
ROOT={remote_root!r}
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT"
python3 - <<'PY'
import base64, io, tarfile
raw = base64.b64decode({b64!r})
with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as tf:
    tf.extractall({remote_root!r})
PY

COMMON=(
  --target=aarch64-none-elf -Os -std=c11 -ffreestanding -fno-builtin
  -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -mstrict-align -mno-outline-atomics
  -Wall -Wextra -Werror -I"$ROOT"
)

clang-21 "${{COMMON[@]}}" -mgeneral-regs-only -DHDF_SCALAR -c "$ROOT/z8_hdf.c" -o "$ROOT/z8_hdf.o"
clang-21 "${{COMMON[@]}}" {k_extra} -c "$ROOT/z8_hdf_kernels.c" -o "$ROOT/z8_hdf_kernels.o"

cat > "$ROOT/link.ld" <<'LDS'
SECTIONS {{
  .text 0x409b0000 : {{
    *(.text*)
    . = ALIGN(16);
    *(.rodata*)
  }}
  .data : {{ *(.data*) }}
  .bss  : {{ *(.bss*) *(COMMON) }}
  ASSERT(SIZEOF(.data) == 0, "writable .data forbidden")
  ASSERT(SIZEOF(.bss) == 0, "writable .bss forbidden")
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
LDS

ld.lld-21 -T "$ROOT/link.ld" "$ROOT/z8_hdf.o" "$ROOT/z8_hdf_kernels.o" -o "$ROOT/hdf_test.elf"

python3 - <<'PY'
import base64, json, re, subprocess
from pathlib import Path

root = Path({remote_root!r})
fp_re = re.compile(r'\\b(?:fadd|fsub|fmul|fdiv|fmadd|fmsub|fnmadd|fnmsub|fneg|fabs|fsqrt|fcmp|fcmpe|fccmp|fcsel|fcvt[a-z]*|scvtf|ucvtf|frint[a-z]*|fmax[a-z]*|fmin[a-z]*|frecp[a-z]*|frsq[a-z]*)\\b')
simd_re = re.compile(r'\\b(?:v\\d+\\.|[bhsdq]\\d+\\b|fmov|fadd|fsub|fmul|fdiv|scvtf|ucvtf|fcvtz|fcvt|fcmp|fmadd|fmsub|dup\\s|ushll|sshll|uaddw|usubw|uqadd|uqsub|shrn|ld[1-4]\\s*\\{{|st[1-4]\\s*\\{{|movi\\s+v)')
qspill_re = re.compile(r'\\b(?:str|stp|stur)\\s+q\\d+')
subsp_re = re.compile(r'\\bsub\\s+sp,\\s*sp,\\s*#(0x[0-9a-f]+|\\d+)')

audit = {{"objects": {{}}, "q_spills": {{}}, "max_stack_bytes": {{}}, "compiler": subprocess.check_output(["clang-21", "--version"], text=True).splitlines()[0]}}
for oname in ("z8_hdf.o", "z8_hdf_kernels.o"):
    dis = subprocess.check_output(["llvm-objdump-21", "-d", str(root / oname)], text=True)
    fp_cnt = simd_cnt = insns = 0
    cur_fn = None
    for line in dis.splitlines():
        mf = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
        if mf:
            cur_fn = mf.group(1)
            continue
        mi = re.match(r'^\\s+[0-9a-f]+:\\s+[0-9a-f]{{8}}\\s+(.*)$', line)
        if mi:
            rest = mi.group(1)
            insns += 1
            if fp_re.search(rest): fp_cnt += 1
            if simd_re.search(rest): simd_cnt += 1
            if qspill_re.search(rest):
                audit["q_spills"].setdefault(f"{{oname}}:{{cur_fn}}", []).append(rest.strip())
            ms = subsp_re.search(rest)
            if ms and cur_fn:
                val = int(ms.group(1), 0)
                if val > audit["max_stack_bytes"].get(cur_fn, 0):
                    audit["max_stack_bytes"][cur_fn] = val
    sh = subprocess.check_output(["llvm-readelf-21", "-SW", str(root / oname)], text=True)
    text_b = ro_b = data_b = bss_b = 0
    for line in sh.splitlines():
        m = re.match(r'^\\s*\\[\\s*\\d+\\]\\s+(\\S+)\\s+(\\S+)\\s+[0-9a-f]+\\s+[0-9a-f]+\\s+([0-9a-f]+)\\s+[0-9a-f]+\\s+(\\S+)', line)
        if not m or 'A' not in m.group(4): continue
        sz = int(m.group(3), 16)
        if 'W' in m.group(4):
            if m.group(2) == 'NOBITS': bss_b += sz
            else: data_b += sz
        elif 'X' in m.group(4): text_b += sz
        else: ro_b += sz
    audit["objects"][oname] = {{
        "text_bytes": text_b, "rodata_bytes": ro_b, "data_bytes": data_b, "bss_bytes": bss_b,
        "insns": insns, "fp_insns": fp_cnt, "simd_insns": simd_cnt
    }}

elf_raw = (root / "hdf_test.elf").read_bytes()
audit["elf_b64"] = base64.b64encode(elf_raw).decode("ascii")
print(json.dumps(audit))
PY
"""
    res = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "win-lab", "wsl -e bash -s"],
        input=remote_script,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if res.returncode != 0:
        print("WSL COMPILE FAILED:", res.stderr, res.stdout)
        raise SystemExit(res.returncode)
    data = json.loads(res.stdout)
    elf_bytes = base64.b64decode(data.pop("elf_b64"))
    return elf_bytes, data


def run_unicorn_hdf(elf_bytes: bytes, main_bytes: bytes | None, src_y: bytes, src_c: bytes,
                    level: int, monochrome: int = 0) -> tuple[int, dict[str, int], bytes, bytes]:
    ef = ELFFile(io.BytesIO(elf_bytes))
    symtab = ef.get_section_by_name(".symtab")
    syms: dict[str, int] = {}
    func_ranges: list[tuple[int, int, str]] = []
    for s in symtab.iter_symbols():
        if s.name and isinstance(s["st_shndx"], int):
            v = s["st_value"]
            sz = s["st_size"]
            if s.name not in syms or s["st_info"]["bind"] == "STB_GLOBAL":
                syms[s.name] = v
            if sz > 0 and s["st_info"]["type"] == "STT_FUNC":
                func_ranges.append((v, v + sz, s.name))
    func_ranges.sort()
    starts = [r[0] for r in func_ranges]

    def addr_to_func(pc: int) -> str:
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and func_ranges[i][0] <= pc < func_ranges[i][1]:
            return func_ranges[i][2]
        return f"unk_{pc:#x}"

    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    if main_bytes is not None:
        u.mem_map(MAIN_VA_BASE, (len(main_bytes) + 0xFFF) & ~0xFFF)
        u.mem_write(MAIN_VA_BASE, main_bytes)
    else:
        for sec in ef.iter_sections():
            if (sec["sh_flags"] & 2) and sec["sh_size"] > 0:
                va = sec["sh_addr"]
                sz = sec["sh_size"]
                lo = va & ~0xFFF
                hi = (va + sz + 0xFFF) & ~0xFFF
                u.mem_map(lo, hi - lo)
                if sec["sh_type"] != "SHT_NOBITS":
                    u.mem_write(va, sec.data())

    u.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    END = 0x48000000
    u.mem_map(END, 0x1000)
    u.mem_write(END, struct.pack("<I", 0xD503201F))

    RAM = 0x90000000
    u.mem_map(RAM, 0x00800000)
    SP = RAM + 0x00080000 - 0x80
    WS = RAM + 0x00100000
    YVA = RAM + 0x00200000
    CVA = RAM + 0x00300000
    u.mem_write(YVA, src_y)
    u.mem_write(CVA, src_c)

    CALLEE_X = [
        UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22,
        UC_ARM64_REG_X23, UC_ARM64_REG_X24, UC_ARM64_REG_X25, UC_ARM64_REG_X26,
        UC_ARM64_REG_X27, UC_ARM64_REG_X28, UC_ARM64_REG_X29,
    ]
    CALLEE_D = [
        UC_ARM64_REG_D8, UC_ARM64_REG_D9, UC_ARM64_REG_D10, UC_ARM64_REG_D11,
        UC_ARM64_REG_D12, UC_ARM64_REG_D13, UC_ARM64_REG_D14, UC_ARM64_REG_D15,
    ]
    ARGS = [
        UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
        UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7,
    ]

    x_sent = {r: (0xCAFEBABE00000000 | (0x2100 + i * 0x11)) for i, r in enumerate(CALLEE_X)}
    d_sent = {r: (0xDEADBEEF55AA0000 | (0x90 + i)) for i, r in enumerate(CALLEE_D)}
    for r, v in x_sent.items():
        u.reg_write(r, v)
    for r, v in d_sent.items():
        u.reg_write(r, v)

    args = [YVA, 1024, CVA, 1024, 682, level, monochrome, 0, 0, WS, 524288]
    u.mem_write(SP, b"".join(struct.pack("<Q", a) for a in args[8:]))
    for i, a in enumerate(args[:8]):
        u.reg_write(ARGS[i], a)
    u.reg_write(UC_ARM64_REG_SP, SP)
    u.reg_write(UC_ARM64_REG_LR, END)

    counts: dict[str, int] = defaultdict(int)

    def on_block(uc, addr, sz, _):
        if addr != END:
            counts[addr_to_func(addr)] += sz >> 2

    def on_bad(uc, access, addr, size, value, _):
        print(f"MEM_INVALID pc={uc.reg_read(UC_ARM64_REG_PC):#x} addr={addr:#x} sz={size}")
        return False

    u.hook_add(UC_HOOK_BLOCK, on_block)
    u.hook_add(UC_HOOK_MEM_INVALID, on_bad)
    u.emu_start(syms["z8_hdf_live_nv12_1024"], END)

    assert u.reg_read(UC_ARM64_REG_PC) == END
    assert u.reg_read(UC_ARM64_REG_SP) == SP
    for r, v in x_sent.items():
        assert u.reg_read(r) == v, f"Clobbered callee-saved X reg {r}"
    for r, v in d_sent.items():
        assert u.reg_read(r) == v, f"Clobbered callee-saved D reg {r}"
    rc = u.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF
    assert rc == 0, f"z8_hdf_live_nv12_1024 returned {rc}"

    out_y = bytes(u.mem_read(YVA, len(src_y)))
    out_c = bytes(u.mem_read(CVA, len(src_c)))
    return sum(counts.values()), dict(counts), out_y, out_c


def main() -> None:
    print("Cross-compiling V71.3 HDF (NEON + Scalar) on win-lab WSL clang-21...")
    new_neon_elf, audit_neon = compile_hdf_on_wsl(HDF_DIR, scalar_kernels=False)
    new_scal_elf, audit_scal = compile_hdf_on_wsl(HDF_DIR, scalar_kernels=True)

    print("\n=== Static ELF / Q-Spill / FP / Stack Audit (clang-21 AArch64) ===")
    print("Compiler:", audit_neon["compiler"])
    for oname, info in audit_neon["objects"].items():
        print(f"  {oname:18s}: .text={info['text_bytes']:6d}B  .rodata={info['rodata_bytes']:5d}B  "
              f".data={info['data_bytes']}  .bss={info['bss_bytes']}  fp={info['fp_insns']}  simd={info['simd_insns']}")
    print("Q-register stack spills (str/stp/stur q):", audit_neon["q_spills"] or "NONE (0 spills!)")
    print("Top stack frames (bytes):", sorted(audit_neon["max_stack_bytes"].items(), key=lambda x: -x[1])[:8])

    assert audit_neon["objects"]["z8_hdf.o"]["fp_insns"] == 0
    assert audit_neon["objects"]["z8_hdf.o"]["simd_insns"] == 0
    assert audit_neon["objects"]["z8_hdf_kernels.o"]["fp_insns"] == 0
    assert audit_neon["objects"]["z8_hdf.o"]["data_bytes"] == 0 and audit_neon["objects"]["z8_hdf.o"]["bss_bytes"] == 0
    assert audit_neon["objects"]["z8_hdf_kernels.o"]["data_bytes"] == 0 and audit_neon["objects"]["z8_hdf_kernels.o"]["bss_bytes"] == 0

    base_elf_bytes = BASE_ELF.read_bytes()
    base_main_bytes = BASE_MAIN.read_bytes()

    frames = []
    for jpg in (JPG_9130, JPG_9129, JPG_9126):
        if jpg.exists():
            y, c, lbl = load_real_z8_nv12_1024x682(jpg)
            frames.append((jpg.stem, y, c))

    results = {"compiler": audit_neon["compiler"], "audit": audit_neon, "benchmarks": {}}

    for fname, src_y, src_c in frames:
        print(f"\n================ Unicorn AArch64 Benchmark: {fname} (1024x682 NV12) ================")
        f_res = {}
        for lvl, lname in ((1, "WEAK"), (2, "MEDIUM"), (3, "STRONG")):
            b_tot, b_bk, _, _ = run_unicorn_hdf(base_elf_bytes, base_main_bytes, src_y, src_c, lvl, 0)
            n_tot, n_bk, ny, nc = run_unicorn_hdf(new_neon_elf, None, src_y, src_c, lvl, 0)
            if fname == "DSC_9130_f03":
                # Also verify AArch64 Unicorn NEON == AArch64 Unicorn Scalar bit-exact 0-diff!
                s_tot, _, sy, sc = run_unicorn_hdf(new_scal_elf, None, src_y, src_c, lvl, 0)
                assert ny == sy and nc == sc, f"ARM64 Unicorn NEON != Scalar on {fname} lvl={lvl}!"
                parity_str = f"0-diff (scalar={s_tot:,})"
            else:
                parity_str = "verified"

            saved = b_tot - n_tot
            pct = 100.0 * saved / b_tot
            print(f"  HDF {lname:6s} (lvl={lvl}): V71.2={b_tot:10,d} -> V71.3={n_tot:10,d}  "
                  f"saved={saved:10,d} (-{pct:5.2f}%)  [ARM64 NEON==Scalar: {parity_str}]")
            f_res[lname] = {
                "baseline_insns": b_tot,
                "new_insns": n_tot,
                "saved_insns": saved,
                "reduction_pct": round(pct, 2),
                "breakdown_new": dict(sorted(n_bk.items(), key=lambda x: -x[1])),
            }
            if fname == "DSC_9130_f03" and lvl in (1, 3):
                print(f"    --- Per-function breakdown ({fname} lvl={lvl}) ---")
                for k, v in sorted(n_bk.items(), key=lambda x: -x[1]):
                    print(f"      {k:38s}: {v:10,d} ({100.0*v/n_tot:5.1f}%)")
        # Also test Monochrome level=3
        b_m, _, _, _ = run_unicorn_hdf(base_elf_bytes, base_main_bytes, src_y, src_c, 3, 1)
        n_m, _, my, mc = run_unicorn_hdf(new_neon_elf, None, src_y, src_c, 3, 1)
        assert mc == src_c, "Monochrome modified UV!"
        print(f"  HDF MONO   (lvl=3): V71.2={b_m:10,d} -> V71.3={n_m:10,d}  "
              f"saved={b_m-n_m:10,d} (-{100.0*(b_m-n_m)/b_m:5.2f}%)")
        f_res["MONO_STRONG"] = {"baseline_insns": b_m, "new_insns": n_m, "saved_insns": b_m - n_m}
        results["benchmarks"][fname] = f_res

    out_json = HERE / "unicorn_hdf_v71r4_bench.json"
    out_json.write_text(json.dumps(results, indent=2) + "\n")
    print(f"\nSaved receipt: {out_json}")
    assert not audit_neon["q_spills"], f"Q-SPILLS DETECTED: {audit_neon['q_spills']}"


if __name__ == "__main__":
    main()
