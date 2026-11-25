#!/usr/bin/env bash
# Build & run Mac Host ASan+UBSan test suite for V71.7 LUT Core & XM6 Hardening,
# re-run existing V71.6-XM6 bridge host acceptance tests (NEON + SCALAR),
# and perform bare-metal AArch64 EL1 static audit (0 .data/.bss, 0 non-kernel FP/SIMD).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V71R7_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FILMSIM_DIR="${V71R7_ROOT}/work/filmsim"

CC="${CC:-clang}"
TMP_DIR="$(mktemp -d /tmp/v71r7_lut_core_test.XXXXXX)"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "=== [1/3] Building & running V71.7 LUT Core + XM6 Hardening Host Suite (ASan + UBSan) ==="
HOST_CFLAGS=(
    -std=c11 -O2 -g
    -Wall -Wextra -Werror
    -fsanitize=address,undefined -fno-omit-frame-pointer
    -pthread
    -DFILMSIM_HOST_TEST=1
    -DV71R7_HOST_TEST=1
    "-I${SCRIPT_DIR}"
    "-I${FILMSIM_DIR}/src/include"
    "-I${FILMSIM_DIR}/src/xm6"
    "-I${FILMSIM_DIR}/src/photo"
    "-I${FILMSIM_DIR}/src/pipeline"
)

HOST_SRCS=(
    "${SCRIPT_DIR}/test_lut_core_host.c"
    "${SCRIPT_DIR}/cube.c"
    "${SCRIPT_DIR}/cube_frame.c"
    "${SCRIPT_DIR}/cube_frame_neon.c"
    "${SCRIPT_DIR}/layer0_color.c"
    "${SCRIPT_DIR}/layer0_color_neon.c"
    "${SCRIPT_DIR}/neon_sentinel.c"
    "${SCRIPT_DIR}/photo_flight.c"
    "${SCRIPT_DIR}/smp.c"
    "${SCRIPT_DIR}/lut_runtime.c"
    "${FILMSIM_DIR}/src/xm6/z8_xm6_bridge.c"
    "${FILMSIM_DIR}/src/photo/z8_native_photo_platform.c"
)

"${CC}" "${HOST_CFLAGS[@]}" "${HOST_SRCS[@]}" -o "${TMP_DIR}/test_lut_core_host"
"${TMP_DIR}/test_lut_core_host"

echo ""
echo "=== [2/3] Re-running existing XM6 Bridge Host Acceptance Tests (NEON + SCALAR) ==="
bash "${FILMSIM_DIR}/tests/run_xm6_bridge_test.sh"

echo ""
echo "=== [3/3] Bare-Metal AArch64 EL1 Compile & Zero-.data/.bss / Zero-Scalar-SIMD Audit ==="
EL1_OBJ_DIR="${TMP_DIR}/el1_objs"
mkdir -p "${EL1_OBJ_DIR}"

EL1_COMMON=(
    --target=aarch64-none-elf
    -Os
    -std=c11
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -ffunction-sections
    -fdata-sections
    -mstrict-align
    -mno-outline-atomics
    -Wall -Wextra -Werror
    "-I${SCRIPT_DIR}"
    "-I${V71R7_ROOT}/work/src-fix37/codec/freestanding"
    "-I${FILMSIM_DIR}/src/include"
    "-I${FILMSIM_DIR}/src/xm6"
    "-I${FILMSIM_DIR}/src/photo"
    "-I${FILMSIM_DIR}/src/pipeline"
)

SCALAR_UNITS=(
    "${SCRIPT_DIR}/cube.c"
    "${SCRIPT_DIR}/cube_frame.c"
    "${SCRIPT_DIR}/layer0_color.c"
    "${SCRIPT_DIR}/neon_sentinel.c"
    "${SCRIPT_DIR}/photo_flight.c"
    "${SCRIPT_DIR}/smp.c"
    "${SCRIPT_DIR}/lut_runtime.c"
    "${FILMSIM_DIR}/src/xm6/z8_xm6_bridge.c"
    "${FILMSIM_DIR}/src/photo/z8_native_photo_platform.c"
)

NEON_UNITS=(
    "${SCRIPT_DIR}/cube_frame_neon.c"
    "${SCRIPT_DIR}/layer0_color_neon.c"
)

for src in "${SCALAR_UNITS[@]}"; do
    base="$(basename "${src}")"
    "${CC}" "${EL1_COMMON[@]}" -mgeneral-regs-only -c "${src}" -o "${EL1_OBJ_DIR}/${base}.o"
done

for src in "${NEON_UNITS[@]}"; do
    base="$(basename "${src}")"
    "${CC}" "${EL1_COMMON[@]}" -c "${src}" -o "${EL1_OBJ_DIR}/${base}.o"
done

python3 - "${EL1_OBJ_DIR}" <<'PY'
import os, re, struct, subprocess, sys

obj_dir = sys.argv[1]
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXEC  = 0x4
SHN_COMMON = 0xfff2

scalar_names = {
    "cube.c.o", "cube_frame.c.o", "layer0_color.c.o", "neon_sentinel.c.o",
    "photo_flight.c.o", "smp.c.o", "lut_runtime.c.o",
    "z8_xm6_bridge.c.o", "z8_native_photo_platform.c.o",
}

def audit_elf(path):
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 2, f"Not ELF64: {path}"
    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 58)
    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        shdrs.append(struct.unpack_from("<IIQQQQIIQQ", data, off))
    shstr_off = shdrs[e_shstrndx][4]
    def shname(idx):
        end = data.index(b"\x00", shstr_off + idx)
        return data[shstr_off + idx:end].decode()

    text_sz = 0
    ro_sz = 0
    wr_sz = 0
    wr_secs = []
    common_syms = []
    for sh_name, sh_type, sh_flags, sh_addr, sh_off, sh_size, sh_link, sh_info, sh_align, sh_entsize in shdrs:
        name = shname(sh_name)
        if sh_flags & SHF_ALLOC:
            if sh_flags & SHF_WRITE:
                wr_sz += sh_size
                if sh_size > 0:
                    wr_secs.append((name, sh_size))
            elif sh_flags & SHF_EXEC:
                text_sz += sh_size
            else:
                ro_sz += sh_size
        if sh_type == 2:  # SHT_SYMTAB
            str_off = shdrs[sh_link][4]
            n = sh_size // sh_entsize
            for j in range(n):
                st_name, st_info, st_other, st_shndx, st_val, st_size = struct.unpack_from("<IBBHQQ", data, sh_off + j * sh_entsize)
                if st_shndx == SHN_COMMON:
                    end = data.index(b"\x00", str_off + st_name)
                    common_syms.append(data[str_off + st_name:end].decode())
    return text_sz, ro_sz, wr_sz, wr_secs, common_syms

def count_fp_simd(path):
    out = subprocess.check_output(["objdump", "-d", path], text=True, stderr=subprocess.DEVNULL)
    # Match any AArch64 vector/FP register operand (v0..v31, q0..q31, d0..d31, s0..s31, h0..h31, b0..b31)
    pat = re.compile(r"(?:^|[\s,{])([vqds]\d{1,2}(?:\.\d+[bhsd])?|fpcr|fpsr)(?:[\s,}\[]|$)")
    hits = 0
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        insn = " ".join(parts[2:])
        if pat.search(insn):
            hits += 1
    return hits

total_lut_text = 0
total_lut_ro = 0
print(f"{'Unit':32s} {'Text':>7s} {'Rodata':>7s} {'Write':>6s} {'COMMON':>6s} {'FP/SIMD':>7s}")
print("-" * 72)
for fname in sorted(os.listdir(obj_dir)):
    p = os.path.join(obj_dir, fname)
    text_sz, ro_sz, wr_sz, wr_secs, commons = audit_elf(p)
    fp_simd = count_fp_simd(p)
    is_scalar = fname in scalar_names
    print(f"{fname:32s} {text_sz:7d} {ro_sz:7d} {wr_sz:6d} {len(commons):6d} {fp_simd:7d}")
    assert wr_sz == 0 and not wr_secs, f"Writable section in {fname}: {wr_secs}"
    assert not commons, f"COMMON symbol in {fname}: {commons}"
    if is_scalar:
        assert fp_simd == 0, f"FP/SIMD instruction detected in scalar unit {fname}: {fp_simd}"
    if not fname.startswith("z8_"):
        total_lut_text += text_sz
        total_lut_ro += ro_sz

print("-" * 72)
print(f"{'lut_core total (Bank 1 RX+RO)':32s} {total_lut_text:7d} {total_lut_ro:7d}      0      0   (NEON kernels only)")
print(f"Total lut_core RX+RO = {total_lut_text + total_lut_ro} B ({(total_lut_text + total_lut_ro)/1024:.2f} KiB) vs 68 KiB Bank 1 budget")
PY

echo ""
echo "=== ALL V71.7 LUT CORE & XM6 HARDENING TESTS + EL1 AUDITS PASSED ==="
