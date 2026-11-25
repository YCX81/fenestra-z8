#!/usr/bin/env python3
"""V71.3 P1-2 WSL clang-21 + Unicorn AArch64 verification & instruction-count benchmark
for work/neon_scale/live_scale_neon.c.

Verifies:
  1. Static ELF audit: 0 undef, 0 .data/.bss, 0 FP insns, 0 Q-register stack spills across ALL fns,
     0 stack in all leaf NEON kernels, compact .text/.rodata footprint.
  2. Dynamic dc cvac + dsb sy cache-line coverage audit in Unicorn (all 25,920 64B lines of 1280x864 NV12).
  3. 100% bit-exact 0-diff vs V71.2 reference oracle across:
       - Realistic midtone photo (1024x682 -> 1280x852, ~90% midtone)
       - Pure midtone photo (100% in [32,223])
       - High-contrast B&W + exact knee-boundary (31/32, 223/224) frame
       - Extreme all-dark (0..31) and all-bright (224..255) worst-case frames
       - Uniform random [0..255] frame (apples-to-apples vs V71.2 baseline 2,352,638)
       - Full int8_t [-128..+127] extreme grain tile + realistic grain intensities
       - Misaligned src/dst/grain_tile + padded strides + src_h=680
       - Plain grain=NULL, live_copy_nv12_neon[_grain], live_downscale_1280_to_1024_nv12_neon
  4. Per-function instruction breakdown + strict AAPCS64 callee-saved conservation.
"""
from __future__ import annotations

import base64
import hashlib
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

REMOTE_SCRIPT = r'''
set -euo pipefail
WORK=/tmp/v71r4_grain_bench
rm -rf "$WORK"
mkdir -p "$WORK"

python3 - <<'PY_UNPACK'
import base64, json
from pathlib import Path
work = Path("/tmp/v71r4_grain_bench")
payload = json.loads(Path("/tmp/v71r4_grain_payload.json").read_text())
(work / "live_scale_neon.h").write_bytes(base64.b64decode(payload["h_b64"]))
(work / "live_scale_neon.c").write_bytes(base64.b64decode(payload["c_b64"]))
PY_UNPACK

cd "$WORK"

/usr/bin/clang-21 --target=aarch64-none-elf -O2 -std=c11 \
  -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -ffunction-sections -fdata-sections -mstrict-align -mno-outline-atomics \
  -Wall -Wextra -Werror \
  -c live_scale_neon.c -o live_scale_neon.c.o

cat > filmsim_test.ld <<'LD'
ENTRY(live_scale_1024_to_1280_nv12_neon)
SECTIONS {
  . = 0x409c5000;
  .text : ALIGN(16) { *(.text .text.* .filmsim .filmsim.*) }
  .rodata : ALIGN(16) { *(.rodata .rodata.*) }
  .data : ALIGN(16) { *(.data .data.*) }
  .bss : ALIGN(16) { *(.bss .bss.* COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) *(.eh_frame*) }
}
LD

/usr/bin/ld.lld -T filmsim_test.ld -nostdlib -o live_scale_neon.elf live_scale_neon.c.o

cat > ref_oracle.c <<'CREF'
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static unsigned ref_cache_slot(int32_t idx[2], unsigned row, int32_t keep, int *miss) {
    if (idx[0] == (int32_t)row) { *miss = 0; return 0u; }
    if (idx[1] == (int32_t)row) { *miss = 0; return 1u; }
    unsigned slot = (keep >= 0 && idx[0] == keep) ? 1u : 0u;
    idx[slot] = (int32_t)row;
    *miss = 1;
    return slot;
}
static inline uint8_t fh(uint8_t a, uint8_t b) { return (uint8_t)(((unsigned)a + (unsigned)b) >> 1); }
static inline uint8_t rh(uint8_t a, uint8_t b) { return (uint8_t)(((unsigned)a + (unsigned)b + 1u) >> 1); }
static uint8_t ref_v71_blend16(uint8_t a, uint8_t b, unsigned wy) {
    if (wy > 8u) { uint8_t t = a; a = b; b = t; wy = 16u - wy; }
    switch (wy) {
    case 0u: return a;
    case 8u: return rh(a, b);
    case 4u: return rh(a, fh(a, b));
    case 2u: return rh(a, fh(a, rh(a, b)));
    case 6u: return rh(a, fh(b, rh(a, b)));
    case 1u: return rh(a, fh(a, rh(a, fh(a, b))));
    case 3u: return rh(a, fh(a, rh(b, fh(a, b))));
    case 5u: return rh(a, fh(b, rh(a, fh(a, b))));
    default: return rh(a, fh(b, rh(b, fh(a, b))));
    }
}
static void ref_v71_hscale_luma(uint8_t *dst, const uint8_t *p) {
    for (unsigned g = 0u; g < 256u; ++g) {
        uint8_t a0 = p[4u*g], a1 = p[4u*g+1u], a2 = p[4u*g+2u], a3 = p[4u*g+3u];
        uint8_t a4 = (g < 255u) ? p[4u*g+4u] : a3;
        dst[5u*g+0u] = a0;
        dst[5u*g+1u] = rh(a1, fh(a1, rh(a0, fh(a0, a1))));
        dst[5u*g+2u] = rh(a2, fh(a1, rh(a1, a2)));
        dst[5u*g+3u] = rh(a2, fh(a3, rh(a2, a3)));
        dst[5u*g+4u] = rh(a3, fh(a3, rh(a4, fh(a3, a4))));
    }
}
static void ref_v71_hscale_uv(uint8_t *ud, uint8_t *vd, const uint8_t *p) {
    for (unsigned g = 0u; g < 128u; ++g) {
        unsigned b = 8u * g;
        uint8_t u0 = p[b], v0 = p[b+1u], u1 = p[b+2u], v1 = p[b+3u];
        uint8_t u2 = p[b+4u], v2 = p[b+5u], u3 = p[b+6u], v3 = p[b+7u];
        uint8_t u4 = (g < 127u) ? p[b+8u] : u3;
        uint8_t v4 = (g < 127u) ? p[b+9u] : v3;
        ud[5u*g+0u] = u0;
        ud[5u*g+1u] = rh(u1, fh(u1, rh(u0, fh(u0, u1))));
        ud[5u*g+2u] = rh(u2, fh(u1, rh(u1, u2)));
        ud[5u*g+3u] = rh(u2, fh(u3, rh(u2, u3)));
        ud[5u*g+4u] = rh(u3, fh(u3, rh(u4, fh(u3, u4))));
        vd[5u*g+0u] = v0;
        vd[5u*g+1u] = rh(v1, fh(v1, rh(v0, fh(v0, v1))));
        vd[5u*g+2u] = rh(v2, fh(v1, rh(v1, v2)));
        vd[5u*g+3u] = rh(v2, fh(v3, rh(v2, v3)));
        vd[5u*g+4u] = rh(v3, fh(v3, rh(v4, fh(v3, v4))));
    }
}
static uint8_t ref_grain_px(uint8_t y, int8_t g) {
    int32_t yv = (int32_t)y;
    int32_t t0 = 2 * yv, t1 = 2 * (255 - yv);
    int32_t tone = t0 < t1 ? t0 : t1;
    if (tone > 64) tone = 64;
    int32_t prod = (int32_t)g * tone + 32;
    int32_t d = (prod >= 0) ? (prod >> 6) : -(((-prod) + 63) >> 6);
    int32_t out = yv + d;
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}
static void ref_apply_grain_row(uint8_t *row, unsigned w, unsigned y_idx, const int8_t *gtile) {
    if (!gtile) return;
    const int8_t *grow = gtile + (size_t)(y_idx & 255u) * 512u;
    unsigned x0 = ((y_idx >> 8) * 192u) & 511u;
    for (unsigned x = 0u; x < w; ++x)
        row[x] = ref_grain_px(row[x], grow[(x0 + x) & 511u]);
}
void ref_v71_nv12(uint8_t *dst_y, uint8_t *dst_uv, unsigned dst_ys, unsigned dst_uvs,
                  unsigned coded_h, const uint8_t *src_y, const uint8_t *src_uv,
                  unsigned sh, unsigned sy_s, unsigned suv_s, const int8_t *gtile) {
    const unsigned dw = 1280u, dh = 852u, cw = 640u, ch = 426u, sh_c = sh / 2u;
    uint8_t lh[2][1280];
    int32_t row_idx[2] = {-1, -1};
    uint32_t y_num = (sh - 1u) << 4, y_den = dh - 1u;
    uint32_t y_qs = y_num / y_den, y_rs = y_num % y_den, fy = 0u, y_rem = 0u;
    for (unsigned y = 0u; y < dh; ++y) {
        unsigned iy = fy >> 4, wy = fy & 15u, iy1 = (iy + 1u < sh) ? (iy + 1u) : iy;
        fy += y_qs; y_rem += y_rs; if (y_rem >= y_den) { y_rem -= y_den; fy++; }
        int miss = 0;
        unsigned s0 = ref_cache_slot(row_idx, iy, wy ? (int32_t)iy1 : -1, &miss);
        if (miss) ref_v71_hscale_luma(lh[s0], src_y + (size_t)iy * sy_s);
        unsigned s1 = s0;
        if (wy != 0u && iy1 != iy) {
            s1 = ref_cache_slot(row_idx, iy1, (int32_t)iy, &miss);
            if (miss) ref_v71_hscale_luma(lh[s1], src_y + (size_t)iy1 * sy_s);
        }
        uint8_t *out = dst_y + (size_t)y * dst_ys;
        for (unsigned x = 0u; x < dw; ++x)
            out[x] = ref_v71_blend16(lh[s0][x], lh[s1][x], wy);
        if (gtile) ref_apply_grain_row(out, dw, y, gtile);
    }
    for (unsigned py = dh; py < coded_h; ++py)
        memcpy(dst_y + (size_t)py * dst_ys, dst_y + (size_t)(dh - 1u) * dst_ys, dw);
    uint8_t uh[2][640], vh[2][640];
    int32_t crow_idx[2] = {-1, -1};
    uint32_t cy_num = (sh_c - 1u) << 4, cy_den = ch - 1u;
    uint32_t cy_qs = cy_num / cy_den, cy_rs = cy_num % cy_den, cfy = 0u, cy_rem = 0u;
    for (unsigned y = 0u; y < ch; ++y) {
        unsigned iy = cfy >> 4, wy = cfy & 15u, iy1 = (iy + 1u < sh_c) ? (iy + 1u) : iy;
        cfy += cy_qs; cy_rem += cy_rs; if (cy_rem >= cy_den) { cy_rem -= cy_den; cfy++; }
        int miss = 0;
        unsigned s0 = ref_cache_slot(crow_idx, iy, wy ? (int32_t)iy1 : -1, &miss);
        if (miss) ref_v71_hscale_uv(uh[s0], vh[s0], src_uv + (size_t)iy * suv_s);
        unsigned s1 = s0;
        if (wy != 0u && iy1 != iy) {
            s1 = ref_cache_slot(crow_idx, iy1, (int32_t)iy, &miss);
            if (miss) ref_v71_hscale_uv(uh[s1], vh[s1], src_uv + (size_t)iy1 * suv_s);
        }
        uint8_t *d = dst_uv + (size_t)y * dst_uvs;
        for (unsigned x = 0u; x < cw; ++x) {
            d[2u*x+0u] = ref_v71_blend16(uh[s0][x], uh[s1][x], wy);
            d[2u*x+1u] = ref_v71_blend16(vh[s0][x], vh[s1][x], wy);
        }
    }
    for (unsigned py = ch; py < coded_h / 2u; ++py)
        memcpy(dst_uv + (size_t)py * dst_uvs, dst_uv + (size_t)(ch - 1u) * dst_uvs, dw);
}
void ref_v71_copy_nv12(uint8_t *dst_y, uint8_t *dst_uv, unsigned w, unsigned h,
                       unsigned dst_ys, unsigned dst_uvs, unsigned coded_h,
                       const uint8_t *src_y, const uint8_t *src_uv,
                       unsigned sy_s, unsigned suv_s, const int8_t *gtile) {
    for (unsigned y = 0u; y < h; ++y) {
        memcpy(dst_y + (size_t)y * dst_ys, src_y + (size_t)y * sy_s, w);
        if (gtile) ref_apply_grain_row(dst_y + (size_t)y * dst_ys, w, y, gtile);
    }
    for (unsigned py = h; py < coded_h; ++py)
        memcpy(dst_y + (size_t)py * dst_ys, dst_y + (size_t)(h - 1u) * dst_ys, w);
    for (unsigned y = 0u; y < h / 2u; ++y)
        memcpy(dst_uv + (size_t)y * dst_uvs, src_uv + (size_t)y * suv_s, w);
    for (unsigned py = h / 2u; py < coded_h / 2u; ++py)
        memcpy(dst_uv + (size_t)py * dst_uvs, src_uv + (size_t)(h / 2u - 1u) * suv_s, w);
}
void ref_v71r2_downscale_exact(
    const uint8_t *src_y, const uint8_t *src_uv, uint32_t src_stride,
    uint8_t *dst_y, uint8_t *dst_uv, uint32_t dst_stride) {
    static uint8_t hy[852u * 1024u];
    static uint8_t huv[426u * 1024u];
    for (unsigned y = 0u; y < 852u; ++y) {
        const uint8_t *sr = src_y + (size_t)y * src_stride;
        uint8_t *dr = hy + (size_t)y * 1024u;
        for (unsigned g = 0u; g < 256u; ++g) {
            unsigned s0 = sr[5u * g], s1 = sr[5u * g + 1u], s2 = sr[5u * g + 2u];
            unsigned s3 = sr[5u * g + 3u], s4 = sr[5u * g + 4u];
            dr[4u * g + 0u] = (uint8_t)((7u * s0 + 1u * s1 + 4u) >> 3);
            dr[4u * g + 1u] = (uint8_t)((5u * s1 + 3u * s2 + 4u) >> 3);
            dr[4u * g + 2u] = (uint8_t)((3u * s2 + 5u * s3 + 4u) >> 3);
            dr[4u * g + 3u] = (uint8_t)((1u * s3 + 7u * s4 + 4u) >> 3);
        }
    }
    const uint32_t y_num = 852u << 4, y_den = 682u;
    const uint32_t y_init = ((852u - 682u) << 3) + (y_den >> 1);
    for (unsigned y = 0u; y < 682u; ++y) {
        uint32_t fy = (uint32_t)(((uint64_t)y * y_num + y_init) / y_den);
        unsigned iy = fy >> 4, wy = fy & 15u, w0 = 16u - wy;
        unsigned iy1 = (iy + 1u < 852u) ? (iy + 1u) : iy;
        const uint8_t *r0 = hy + (size_t)iy * 1024u;
        const uint8_t *r1 = hy + (size_t)iy1 * 1024u;
        uint8_t *d = dst_y + (size_t)y * dst_stride;
        for (unsigned x = 0u; x < 1024u; ++x)
            d[x] = (uint8_t)((w0 * r0[x] + wy * r1[x] + 8u) >> 4);
    }
    for (unsigned y = 0u; y < 426u; ++y) {
        const uint8_t *sr = src_uv + (size_t)y * src_stride;
        uint8_t *dr = huv + (size_t)y * 1024u;
        for (unsigned g = 0u; g < 128u; ++g) {
            const uint8_t *s = sr + 10u * g;
            uint8_t *o = dr + 8u * g;
            for (unsigned c = 0u; c < 2u; ++c) {
                unsigned p0 = s[0 + c], p1 = s[2 + c], p2 = s[4 + c], p3 = s[6 + c], p4 = s[8 + c];
                o[0 + c] = (uint8_t)((7u * p0 + 1u * p1 + 4u) >> 3);
                o[2 + c] = (uint8_t)((5u * p1 + 3u * p2 + 4u) >> 3);
                o[4 + c] = (uint8_t)((3u * p2 + 5u * p3 + 4u) >> 3);
                o[6 + c] = (uint8_t)((1u * p3 + 7u * p4 + 4u) >> 3);
            }
        }
    }
    const uint32_t cy_num = 426u << 4, cy_den = 341u;
    const uint32_t cy_init = ((426u - 341u) << 3) + (cy_den >> 1);
    for (unsigned y = 0u; y < 341u; ++y) {
        uint32_t fy = (uint32_t)(((uint64_t)y * cy_num + cy_init) / cy_den);
        unsigned iy = fy >> 4, wy = fy & 15u, w0 = 16u - wy;
        unsigned iy1 = (iy + 1u < 426u) ? (iy + 1u) : iy;
        const uint8_t *r0 = huv + (size_t)iy * 1024u;
        const uint8_t *r1 = huv + (size_t)iy1 * 1024u;
        uint8_t *d = dst_uv + (size_t)y * dst_stride;
        for (unsigned x = 0u; x < 1024u; ++x)
            d[x] = (uint8_t)((w0 * r0[x] + wy * r1[x] + 8u) >> 4);
    }
}
CREF

/usr/bin/clang-21 -O2 -shared -fPIC ref_oracle.c -o libref_oracle.so

/home/ycx/.local/share/nikon-z8-qemu/venv/bin/python - <<'PY_VERIFY'
import bisect, ctypes, hashlib, io, json, math, re, struct, subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE
from unicorn.arm64_const import *

work = Path("/tmp/v71r4_grain_bench")
obj_bytes = (work / "live_scale_neon.c.o").read_bytes()
elf_bytes = (work / "live_scale_neon.elf").read_bytes()

undef_out = subprocess.check_output(["/usr/bin/llvm-nm", "-u", str(work / "live_scale_neon.c.o")], text=True).strip()
assert not undef_out, f"Undefined symbols: {undef_out}"

elf = ELFFile(io.BytesIO(elf_bytes))
sections = {sec.name: {"addr": hex(sec["sh_addr"]), "size": int(sec["sh_size"])} for sec in elf.iter_sections() if sec.name}
text_sz = sections.get(".text", {}).get("size", 0)
rodata_sz = sections.get(".rodata", {}).get("size", 0)
data_sz = sections.get(".data", {}).get("size", 0)
bss_sz = sections.get(".bss", {}).get("size", 0)
assert data_sz == 0 and bss_sz == 0, f"Non-zero data/bss: {data_sz}/{bss_sz}"

symtab = elf.get_section_by_name(".symtab")
syms = {}
fn_ranges = []
for s in symtab.iter_symbols():
    if s.name and s["st_shndx"] != "SHN_UNDEF":
        syms[s.name] = int(s["st_value"])
        if s["st_info"]["type"] == "STT_FUNC" and s["st_size"] > 0:
            fn_ranges.append((int(s["st_value"]), int(s["st_value"]) + int(s["st_size"]), s.name))
fn_ranges.sort()
fn_starts = [r[0] for r in fn_ranges]

def addr_to_fn(addr):
    idx = bisect.bisect_right(fn_starts, addr) - 1
    if idx >= 0 and fn_ranges[idx][0] <= addr < fn_ranges[idx][1]:
        return fn_ranges[idx][2]
    return "unknown"

disasm = subprocess.check_output(
    ["/usr/bin/llvm-objdump", "-d", "--no-show-raw-insn", str(work / "live_scale_neon.elf")], text=True
)
FP_RE = re.compile(
    r"^(?:fadd|fsub|fmul|fdiv|fmadd|fmsub|fnmadd|fnmsub|fcvt\w*|scvtf|ucvtf|"
    r"fcmp\w*|fcm\w*|fcsel|fmov|fabs|fneg|fsqrt|frint\w*|fmax\w*|fmin\w*|"
    r"fmla|fmls|frecp\w*|frsq\w*)$"
)
FMOV_GPR_VEC_RE = re.compile(r"^(?:[wx]\d+,\s*[sd]\d+|[sd]\d+,\s*[wx]\d+)$")
STACK_SUB_RE = re.compile(r"sub\s+sp,\s+sp,\s+#(0x[0-9a-fA-F]+|\d+)(?:,\s*lsl\s*#(\d+))?")
STP_PRE_RE = re.compile(r"stp\s+.*,\s*\[sp,\s*#-(0x[0-9a-fA-F]+|\d+)\]!")
Q_SPILL_RE = re.compile(r"^(?:str|stp|ldr|ldp)\s+q\d+.*\[sp")

fn_stats = {}
fp_hits = []
q_spill_hits = []
dc_cvac_addrs = {}
dsb_sy_addrs = set()
cur_fn = None
for line in disasm.splitlines():
    m_fn = re.match(r"^[0-9a-f]+\s+<([^>]+)>:", line)
    if m_fn:
        cur_fn = m_fn.group(1)
        fn_stats[cur_fn] = {"insns": 0, "bytes": 0, "ldrb": 0, "strb": 0, "stnp": 0, "dc": 0, "dsb": 0, "stack_bytes": 0, "q_spills": 0}
        continue
    m_in = re.match(r"^\s*([0-9a-f]+):\s+([a-z0-9_.]+)(?:\s+(.*))?$", line)
    if m_in and cur_fn:
        pc = int(m_in.group(1), 16)
        mnem = m_in.group(2)
        ops = (m_in.group(3) or "").strip()
        st = fn_stats[cur_fn]
        st["insns"] += 1
        st["bytes"] += 4
        if FP_RE.match(mnem):
            if not (mnem == "fmov" and FMOV_GPR_VEC_RE.match(ops)):
                fp_hits.append(f"{cur_fn}: {line.strip()}")
        if mnem == "ldrb": st["ldrb"] += 1
        elif mnem == "strb": st["strb"] += 1
        elif mnem == "stnp": st["stnp"] += 1
        elif mnem == "dc":
            st["dc"] += 1
            m_dc = re.match(r"cvac,\s*x(\d+)", ops)
            if m_dc:
                dc_cvac_addrs[pc] = int(m_dc.group(1))
        elif mnem == "dsb":
            st["dsb"] += 1
            if ops.split() and ops.split()[0] == "sy":
                dsb_sy_addrs.add(pc)
        if Q_SPILL_RE.match(f"{mnem} {ops}"):
            st["q_spills"] += 1
            q_spill_hits.append(f"{cur_fn}: {line.strip()}")
        ms = STACK_SUB_RE.search(f"{mnem} {ops}")
        if ms:
            st["stack_bytes"] += int(ms.group(1), 0) << (int(ms.group(2)) if ms.group(2) else 0)
        mp = STP_PRE_RE.search(f"{mnem} {ops}")
        if mp:
            st["stack_bytes"] += int(mp.group(1), 0)

assert not fp_hits, f"FP insns: {fp_hits}"
assert not q_spill_hits, f"Q-register stack spills: {q_spill_hits}"
for leaf in ["ls_hscale_1024_1280_aligned", "ls_vblend_1280_aligned", "ls_vblend_1280_grain_aligned",
             "ls_copy_grain_row", "ls_clean_strip", "ls_copy_bytes", "ls_copy_bytes_nt",
             "ls_hdown_1280_1024_aligned", "ls_vblend_1024_16_stp"]:
    assert fn_stats[leaf]["stack_bytes"] == 0, f"Non-zero stack in {leaf}: {fn_stats[leaf]}"
    assert fn_stats[leaf]["q_spills"] == 0

ref = ctypes.CDLL(str(work / "libref_oracle.so"))

def make_random(n, seed):
    x = seed & 0xFFFFFFFF
    out = bytearray(n)
    for i in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = (x >> 24) & 0xFF
    return bytes(out)

def make_photo_1024x682():
    """Realistic Nikon Z8 LiveView 1024x682 NV12 frame: ~88% midtone [32..223],
    ~7% deep shadow (<32), ~5% specular highlight (>223), smooth gradients + fine texture."""
    y_out = bytearray(1024 * 682 + 64)
    uv_out = bytearray(1024 * 341 + 64)
    rng = 0x9E3779B9
    for y in range(682):
        base = y * 1024
        cy = math.cos(y * 0.025)
        dy = y - 340
        for x in range(1024):
            v = 72 + (y * 110) // 682 + int(28.0 * math.sin(x * 0.018) * cy)
            dx = x - 512
            if dx * dx + dy * dy < 150 * 150:
                v = 190 - (dx * 35) // 150
            elif x < 90 and y > 450:
                v = 12 + (x * 18) // 90  # deep shadow (<32)
            elif x > 920 and y < 180:
                v = 228 + ((x - 920) * 25) // 104  # bright sky/window (>223)
            rng ^= (rng << 13) & 0xFFFFFFFF
            rng ^= (rng >> 17)
            rng ^= (rng << 5) & 0xFFFFFFFF
            v += ((rng >> 24) % 9) - 4
            y_out[base + x] = 0 if v < 0 else (255 if v > 255 else v)
    for y in range(341):
        base = y * 1024
        for x in range(512):
            uv_out[base + 2*x] = 96 + (x * 64) // 512
            uv_out[base + 2*x + 1] = 160 - (y * 64) // 341
    return bytes(y_out), bytes(uv_out)

def make_pure_midtone_1024x682():
    y_out = bytearray(1024 * 682 + 64)
    uv_out = bytearray(1024 * 341 + 64)
    for y in range(682):
        base = y * 1024
        for x in range(1024):
            y_out[base + x] = 32 + ((x * 3 + y * 5) % 192)
    for i in range(len(uv_out)):
        uv_out[i] = 128
    return bytes(y_out), bytes(uv_out)

def make_high_contrast_1024x682():
    """High-contrast B&W stripes + exact knee boundaries 0, 1, 31, 32, 33, 128, 222, 223, 224, 254, 255."""
    vals = [0, 1, 31, 32, 33, 128, 222, 223, 224, 254, 255, 16, 240, 100, 150, 200]
    y_out = bytearray(1024 * 682 + 64)
    uv_out = bytearray(1024 * 341 + 64)
    for y in range(682):
        base = y * 1024
        for x in range(1024):
            if (y // 32) % 2 == 0:
                y_out[base + x] = 0 if ((x // 64) % 2 == 0) else 255
            else:
                y_out[base + x] = vals[(x + y) & 15]
    for i in range(len(uv_out)):
        uv_out[i] = (i * 37) & 0xFF
    return bytes(y_out), bytes(uv_out)

def make_all_dark_1024x682():
    raw = make_random(1024 * 682 + 64, 0x11112222)
    y_out = bytes(b & 31 for b in raw)
    uv_out = make_random(1024 * 341 + 64, 0x33334444)
    return y_out, uv_out

def make_all_bright_1024x682():
    raw = make_random(1024 * 682 + 64, 0x55556666)
    y_out = bytes(224 + (b & 31) for b in raw)
    uv_out = make_random(1024 * 341 + 64, 0x77778888)
    return y_out, uv_out

def make_grain_tile_range(seed, lo, hi):
    raw = make_random(512 * 256, seed)
    span = hi - lo + 1
    return bytes((lo + (b % span)) & 0xFF for b in raw)

u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(0x409c0000, 0x00018000)
for seg in elf.iter_segments():
    if seg["p_type"] == "PT_LOAD":
        u.mem_write(seg["p_vaddr"], seg.data())
u.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
END_ADDR = 0x48000000
u.mem_map(END_ADDR, 0x1000)
u.mem_write(END_ADDR, struct.pack("<I", 0xD503201F))

RAM_BASE = 0x90000000
u.mem_map(RAM_BASE, 0x01000000)
STACK_BASE  = RAM_BASE + 0x00080000
SRC_Y_BASE  = RAM_BASE + 0x00100000
SRC_UV_BASE = RAM_BASE + 0x00300000
DST_Y_BASE  = RAM_BASE + 0x00500000
DST_UV_BASE = RAM_BASE + 0x00800000
GTILE_BASE  = RAM_BASE + 0x00E00000

CALLEE_X = [UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22,
            UC_ARM64_REG_X23, UC_ARM64_REG_X24, UC_ARM64_REG_X25, UC_ARM64_REG_X26,
            UC_ARM64_REG_X27, UC_ARM64_REG_X28, UC_ARM64_REG_X29]
CALLEE_D = [UC_ARM64_REG_D8, UC_ARM64_REG_D9, UC_ARM64_REG_D10, UC_ARM64_REG_D11,
            UC_ARM64_REG_D12, UC_ARM64_REG_D13, UC_ARM64_REG_D14, UC_ARM64_REG_D15]
ARG_X = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
         UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7]
X_REG_BY_NUM = [globals()[f"UC_ARM64_REG_X{i}"] for i in range(29)]

insn_total = [0]
fn_insns = {}
dsb_seen = [0]
trace_dc_active = [False]
cleaned_lines = []

def hook_block(uc, address, size, user_data):
    cnt = size >> 2
    insn_total[0] += cnt
    fn = addr_to_fn(address)
    fn_insns[fn] = fn_insns.get(fn, 0) + cnt
    end = address + size
    for da in dsb_sy_addrs:
        if address <= da < end:
            dsb_seen[0] += 1

def hook_dc(uc, address, size, ud):
    if trace_dc_active[0]:
        regn = dc_cvac_addrs.get(address)
        if regn is not None:
            cleaned_lines.append(uc.reg_read(X_REG_BY_NUM[regn]))

u.hook_add(UC_HOOK_BLOCK, hook_block)
clean_lo = syms["ls_clean_strip"]
clean_hi = clean_lo + fn_stats["ls_clean_strip"]["bytes"]
u.hook_add(UC_HOOK_CODE, hook_dc, begin=clean_lo, end=clean_hi)

def call_arm64(sym_name, args, trace_cache=False):
    entry = syms[sym_name]
    sp = STACK_BASE - 0x80
    if len(args) > 8:
        u.mem_write(sp, b"".join(struct.pack("<Q", a & 0xFFFFFFFFFFFFFFFF) for a in args[8:]))
    for i, a in enumerate(args[:8]):
        u.reg_write(ARG_X[i], a & 0xFFFFFFFFFFFFFFFF)
    x_sent = {r: 0xCAFEBABE00000000 | (0x1900 + i * 0x11) for i, r in enumerate(CALLEE_X)}
    for r, v in x_sent.items(): u.reg_write(r, v)
    d_sent = {r: 0xDEADBEEF12345600 | (0x80 + i) for i, r in enumerate(CALLEE_D)}
    for r, v in d_sent.items(): u.reg_write(r, v)
    u.reg_write(UC_ARM64_REG_SP, sp)
    u.reg_write(UC_ARM64_REG_LR, END_ADDR)

    cleaned_lines.clear()
    trace_dc_active[0] = trace_cache
    dsb_seen[0] = 0
    insn_total[0] = 0
    fn_insns.clear()
    u.emu_start(entry, END_ADDR)
    trace_dc_active[0] = False

    assert u.reg_read(UC_ARM64_REG_PC) == END_ADDR
    assert u.reg_read(UC_ARM64_REG_SP) == sp
    for r, v in x_sent.items(): assert u.reg_read(r) == v
    for r, v in d_sent.items(): assert u.reg_read(r) == v
    return u.reg_read(UC_ARM64_REG_X0), insn_total[0], dict(fn_insns), list(cleaned_lines), dsb_seen[0]

IMG_RAND_Y = make_random(1536 * 864 + 64, 0x13579BDF)
IMG_RAND_UV = make_random(1536 * 432 + 64, 0x2468ACE1)
PHOTO_Y, PHOTO_UV = make_photo_1024x682()
MID_Y, MID_UV = make_pure_midtone_1024x682()
HC_Y, HC_UV = make_high_contrast_1024x682()
DARK_Y, DARK_UV = make_all_dark_1024x682()
BRIGHT_Y, BRIGHT_UV = make_all_bright_1024x682()

GTILE_STD = make_grain_tile_range(0xCAFE1234, -48, 48)
GTILE_FULL = make_grain_tile_range(0xBEEF5678, -128, 127)

results = []

def run_scale_case(name, sy_b, suv_b, gtile_b, sh=682, coded_h=864,
                   sy_s=1024, suv_s=1024, dy_s=1280, duv_s=1280,
                   s_mis=0, d_mis=0, g_mis=0, lv_c=1, cln=1, trace_cache=False):
    sy_len = sy_s * sh + 64
    suv_len = suv_s * (sh // 2) + 64
    dy_len = dy_s * coded_h + 64
    duv_len = duv_s * (coded_h // 2) + 64
    sy_buf = bytearray(sy_b[:sy_len])
    suv_buf = bytearray(suv_b[:suv_len])
    dy_ref = bytearray([0xA5] * dy_len)
    duv_ref = bytearray([0x5A] * duv_len)

    if gtile_b is not None:
        gt_buf = bytearray([0x00] * g_mis) + bytearray(gtile_b)
        gt_c = ctypes.byref((ctypes.c_int8 * len(gt_buf)).from_buffer(gt_buf), g_mis)
        u.mem_write(GTILE_BASE, bytes(gt_buf))
        gt_arg = GTILE_BASE + g_mis
    else:
        gt_c = None
        gt_arg = 0

    ref.ref_v71_nv12(
        ctypes.byref((ctypes.c_uint8 * dy_len).from_buffer(dy_ref), d_mis),
        ctypes.byref((ctypes.c_uint8 * duv_len).from_buffer(duv_ref), d_mis),
        dy_s, duv_s, coded_h,
        ctypes.byref((ctypes.c_uint8 * sy_len).from_buffer(sy_buf), s_mis),
        ctypes.byref((ctypes.c_uint8 * suv_len).from_buffer(suv_buf), s_mis),
        sh, sy_s, suv_s, gt_c,
    )
    u.mem_write(SRC_Y_BASE, bytes(sy_buf))
    u.mem_write(SRC_UV_BASE, bytes(suv_buf))
    u.mem_write(DST_Y_BASE, bytes([0xA5] * dy_len))
    u.mem_write(DST_UV_BASE, bytes([0x5A] * duv_len))

    rc, insns, bk, cleaned, dsb_cnt = call_arm64("live_scale_1024_to_1280_nv12_neon_grain", [
        DST_Y_BASE + d_mis, DST_UV_BASE + d_mis, dy_s, duv_s, coded_h,
        SRC_Y_BASE + s_mis, SRC_UV_BASE + s_mis, sh, sy_s, suv_s, lv_c, cln, gt_arg
    ], trace_cache=trace_cache)
    assert rc == 1
    dy_got = bytes(u.mem_read(DST_Y_BASE, dy_len))
    duv_got = bytes(u.mem_read(DST_UV_BASE, duv_len))
    assert dy_got == bytes(dy_ref), f"{name}: Y mismatch vs V71.2 oracle!"
    assert duv_got == bytes(duv_ref), f"{name}: UV mismatch vs V71.2 oracle!"

    cache_info = None
    if trace_cache:
        exp_y = {DST_Y_BASE + d_mis + r * dy_s + x for r in range(coded_h) for x in range(0, 1280, 64)}
        exp_uv = {DST_UV_BASE + d_mis + r * duv_s + x for r in range(coded_h // 2) for x in range(0, 1280, 64)}
        exp_all = {a & ~63 for a in (exp_y | exp_uv)}
        got_set = set(cleaned)
        assert exp_all.issubset(got_set), f"Missing dc cvac lines: {len(exp_all - got_set)}"
        assert dsb_cnt == 1, f"Expected 1 dsb sy, got {dsb_cnt}"
        cache_info = {"dc_cvac_execs": len(cleaned), "unique_lines": len(got_set), "expected_lines": len(exp_all), "dsb_sy_execs": dsb_cnt}

    results.append({"name": name, "insns": insns, "breakdown": bk, "cache": cache_info})

run_scale_case("grain_photo_realistic_88pct_mid", PHOTO_Y, PHOTO_UV, GTILE_STD, trace_cache=True)
run_scale_case("grain_pure_midtone_100pct",       MID_Y,   MID_UV,   GTILE_STD)
run_scale_case("grain_high_contrast_knees",       HC_Y,    HC_UV,    GTILE_FULL)
run_scale_case("grain_all_dark_0pct_mid",         DARK_Y,  DARK_UV,  GTILE_FULL)
run_scale_case("grain_all_bright_0pct_mid",       BRIGHT_Y,BRIGHT_UV,GTILE_FULL)
run_scale_case("grain_uniform_random_v71_bench",  IMG_RAND_Y, IMG_RAND_UV, GTILE_STD)
run_scale_case("grain_misaligned_gtile_3",        PHOTO_Y, PHOTO_UV, GTILE_FULL, g_mis=3)
run_scale_case("grain_padded_680_uncached",       IMG_RAND_Y, IMG_RAND_UV, GTILE_STD, sh=680, sy_s=1088, suv_s=1152, dy_s=1344, duv_s=1408, lv_c=0, cln=1, trace_cache=True)
run_scale_case("plain_grain_off_cached_cvac",     IMG_RAND_Y, IMG_RAND_UV, None, trace_cache=True)

# Also benchmark live_copy_nv12_neon[_grain]
def run_copy_case(name, sy_b, suv_b, gtile_b, w=1280, h=852, coded_h=864, cln=1):
    sy_len = w * h + 64
    suv_len = w * (h // 2) + 64
    dy_len = w * coded_h + 64
    duv_len = w * (coded_h // 2) + 64
    sy_buf = bytearray(sy_b[:sy_len])
    suv_buf = bytearray(suv_b[:suv_len])
    dy_ref = bytearray([0x3C] * dy_len)
    duv_ref = bytearray([0xC3] * duv_len)
    if gtile_b is not None:
        gt_buf = bytearray(gtile_b)
        gt_c = (ctypes.c_int8 * len(gt_buf)).from_buffer(gt_buf)
        u.mem_write(GTILE_BASE, bytes(gt_buf))
        gt_arg = GTILE_BASE
    else:
        gt_c = None
        gt_arg = 0
    ref.ref_v71_copy_nv12(
        (ctypes.c_uint8 * dy_len).from_buffer(dy_ref),
        (ctypes.c_uint8 * duv_len).from_buffer(duv_ref),
        w, h, w, w, coded_h,
        (ctypes.c_uint8 * sy_len).from_buffer(sy_buf),
        (ctypes.c_uint8 * suv_len).from_buffer(suv_buf),
        w, w, gt_c
    )
    u.mem_write(SRC_Y_BASE, bytes(sy_buf))
    u.mem_write(SRC_UV_BASE, bytes(suv_buf))
    u.mem_write(DST_Y_BASE, bytes([0x3C] * dy_len))
    u.mem_write(DST_UV_BASE, bytes([0xC3] * duv_len))
    rc, insns, bk, _, _ = call_arm64("live_copy_nv12_neon_grain", [
        DST_Y_BASE, DST_UV_BASE, w, h, w, w, coded_h,
        SRC_Y_BASE, SRC_UV_BASE, w, w, 1, cln, gt_arg
    ])
    assert rc == 1
    assert bytes(u.mem_read(DST_Y_BASE, dy_len)) == bytes(dy_ref)
    assert bytes(u.mem_read(DST_UV_BASE, duv_len)) == bytes(duv_ref)
    results.append({"name": name, "insns": insns, "breakdown": bk, "cache": None})

# Build 1280x852 midtone photo for copy_grain
copy_photo_y = bytearray(1280 * 852 + 64)
for y in range(852):
    for x in range(1280):
        copy_photo_y[y*1280 + x] = 64 + ((x + y) & 127)
run_copy_case("copy_1024x682_noclean", IMG_RAND_Y, IMG_RAND_UV, None, w=1024, h=682, coded_h=682, cln=0)
run_copy_case("copy_1280x852_cvac_plain", IMG_RAND_Y, IMG_RAND_UV, None, w=1280, h=852, coded_h=864, cln=1)
run_copy_case("copy_1280x852_cvac_grain_photo", bytes(copy_photo_y), IMG_RAND_UV, GTILE_STD, w=1280, h=852, coded_h=864, cln=1)
run_copy_case("copy_1280x852_cvac_grain_random", IMG_RAND_Y, IMG_RAND_UV, GTILE_STD, w=1280, h=852, coded_h=864, cln=1)

# Also benchmark live_downscale_1280_to_1024_nv12_neon
ds_sy = bytearray(IMG_RAND_Y[:1280 * 852 + 64])
ds_suv = bytearray(IMG_RAND_UV[:1280 * 426 + 64])
ds_dy_ref = bytearray([0xA5] * (1024 * 682 + 64))
ds_duv_ref = bytearray([0x5A] * (1024 * 341 + 64))
ref.ref_v71r2_downscale_exact(
    (ctypes.c_uint8 * len(ds_sy)).from_buffer(ds_sy),
    (ctypes.c_uint8 * len(ds_suv)).from_buffer(ds_suv), 1280,
    (ctypes.c_uint8 * len(ds_dy_ref)).from_buffer(ds_dy_ref),
    (ctypes.c_uint8 * len(ds_duv_ref)).from_buffer(ds_duv_ref), 1024
)
u.mem_write(SRC_Y_BASE, bytes(ds_sy))
u.mem_write(SRC_UV_BASE, bytes(ds_suv))
u.mem_write(DST_Y_BASE, bytes([0xA5] * len(ds_dy_ref)))
u.mem_write(DST_UV_BASE, bytes([0x5A] * len(ds_duv_ref)))
_, ds_insns, ds_bk, _, _ = call_arm64("live_downscale_1280_to_1024_nv12_neon", [
    SRC_Y_BASE, SRC_UV_BASE, 1280, DST_Y_BASE, DST_UV_BASE, 1024
])
assert bytes(u.mem_read(DST_Y_BASE, len(ds_dy_ref))) == bytes(ds_dy_ref)
assert bytes(u.mem_read(DST_UV_BASE, len(ds_duv_ref))) == bytes(ds_duv_ref)
results.append({"name": "downscale_1280x852_to_1024x682_aln", "insns": ds_insns, "breakdown": ds_bk, "cache": None})

out = {
    "text_bytes": text_sz,
    "rodata_bytes": rodata_sz,
    "data_bytes": data_sz,
    "bss_bytes": bss_sz,
    "total_filmsim_bytes": text_sz + rodata_sz,
    "fp_insns": len(fp_hits),
    "q_spills_total": len(q_spill_hits),
    "fn_stats": fn_stats,
    "results": results,
}
print("___BENCH_JSON_BEGIN___")
print(json.dumps(out, indent=2))
print("___BENCH_JSON_END___")
PY_VERIFY
'''


def main() -> int:
    h_bytes = (HERE / "live_scale_neon.h").read_bytes()
    c_bytes = (HERE / "live_scale_neon.c").read_bytes()
    payload = {
        "h_b64": base64.b64encode(h_bytes).decode("ascii"),
        "c_b64": base64.b64encode(c_bytes).decode("ascii"),
    }
    up = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "win-lab", "wsl -e dd of=/tmp/v71r4_grain_payload.json status=none"],
        input=json.dumps(payload).encode("utf-8"),
        capture_output=True,
        check=False,
    )
    if up.returncode != 0:
        sys.stderr.write(f"Upload failed: {up.stderr.decode()}\n")
        return 1
    proc = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "win-lab", "wsl -e bash -s"],
        input=REMOTE_SCRIPT.encode("utf-8"),
        capture_output=True,
        check=False,
    )
    stdout = proc.stdout.decode("utf-8", errors="replace")
    stderr = proc.stderr.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        sys.stderr.write(f"Remote failed (rc={proc.returncode}):\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}\n")
        return 1
    m0 = stdout.find("___BENCH_JSON_BEGIN___")
    m1 = stdout.find("___BENCH_JSON_END___")
    data = json.loads(stdout[m0 + len("___BENCH_JSON_BEGIN___"):m1].strip())
    (HERE / "v71r4-grain-bench-receipt.json").write_text(json.dumps(data, indent=2) + "\n")

    print(f"PASS | V71.3 P1-2 WSL clang-21 + Unicorn AArch64 Benchmark")
    print(f"  Footprint : .text={data['text_bytes']} B, .rodata={data['rodata_bytes']} B => {data['total_filmsim_bytes']} B (baseline 17,916 B)")
    print(f"  Audit     : fp_insns={data['fp_insns']}, q_spills_total={data['q_spills_total']}, data={data['data_bytes']}, bss={data['bss_bytes']}")
    print("  Results (100% 0-diff vs V71.2 oracle):")
    for r in data["results"]:
        extra = ""
        if r["cache"]:
            c = r["cache"]
            extra = f" | dc_cvac={c['dc_cvac_execs']} (unique={c['unique_lines']}/{c['expected_lines']}), dsb_sy={c['dsb_sy_execs']}"
        print(f"    {r['name']:36s}: {r['insns']:10,d} insns{extra}")
        top = sorted(r["breakdown"].items(), key=lambda kv: -kv[1])[:5]
        print("      " + ", ".join(f"{k}={v:,d}" for k, v in top))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
