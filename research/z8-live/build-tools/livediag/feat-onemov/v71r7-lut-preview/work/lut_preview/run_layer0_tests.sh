#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# V71.7 Display Layer 0 WYSIWYG Preview, photo_workspace, HangFix V69 & Power Sync
# Local Mac ASan+UBSan Test Suite + AArch64 Zero-Global / Zero-SIMD ELF Audit.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d /tmp/v71r7_layer0_test.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

echo "=== [1/3] Mac Host ASan+UBSan Functional Test Suite ==="
clang -std=c11 -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Werror \
    -DV71R7_HOST_TEST=1 \
    -I"$DIR" -I"$DIR/../lut_core" \
    "$DIR/photo_workspace.c" \
    "$DIR/layer0_provider.c" \
    "$DIR/test_layer0_host.c" \
    -o "$TMP/test_layer0_host"

"$TMP/test_layer0_host"

echo ""
echo "=== [2/3] AArch64 Freestanding Scalar Compilation (-Oz -mgeneral-regs-only) ==="
A64_FLAGS=(
    --target=aarch64-none-elf -Oz -std=c11
    -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -ffunction-sections -fdata-sections
    -mgeneral-regs-only -mstrict-align -mno-outline-atomics
    -Wall -Wextra -Werror
    -I"$DIR" -I"$DIR/../lut_core"
)
clang "${A64_FLAGS[@]}" -c "$DIR/photo_workspace.c" -o "$TMP/photo_workspace.o"
clang "${A64_FLAGS[@]}" -c "$DIR/layer0_provider.c" -o "$TMP/layer0_provider.o"
clang --target=aarch64-none-elf -c "$DIR/lut_hooks.S" -o "$TMP/lut_hooks.o"
echo "  Compiled photo_workspace.o, layer0_provider.o, lut_hooks.o cleanly!"

echo ""
echo "=== [3/3] AArch64 ELF Zero-.data/.bss & Zero-FP/SIMD Audit ==="
python3 - "$TMP/photo_workspace.o" "$TMP/layer0_provider.o" "$TMP/lut_hooks.o" <<'PY'
import re
import struct
import subprocess
import sys
from pathlib import Path

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHN_COMMON = 0xFFF2

vec_re = re.compile(r'(?<![.\w])(?:[bhsdqvzp](?:[12]?\d|3[01])|fpcr|fpsr|ffr)(?!\w)')

total_text = 0
total_rodata = 0
all_undefs = set()
all_defs = set()

for arg in sys.argv[1:]:
    p = Path(arg)
    data = p.read_bytes()
    assert data[:4] == b'\x7fELF' and data[4] == 2 and data[5] == 1, f"{p}: not ELF64 LE"
    e_shoff, = struct.unpack_from('<Q', data, 40)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 58)

    sh_list = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = \
            struct.unpack_from('<IIQQQQIIQQ', data, off)
        sh_list.append((sh_name, sh_type, sh_flags, sh_offset, sh_size, sh_link, sh_entsize))

    str_off = sh_list[e_shstrndx][3]
    def sh_str(idx):
        end = data.index(b'\x00', str_off + idx)
        return data[str_off + idx:end].decode('ascii', errors='replace')

    text_b = 0
    rodata_b = 0
    writable_secs = []
    common_syms = []

    for sh_name, sh_type, sh_flags, sh_offset, sh_size, sh_link, sh_entsize in sh_list:
        name = sh_str(sh_name)
        if (sh_flags & SHF_ALLOC) and (sh_flags & SHF_WRITE) and sh_size > 0:
            writable_secs.append((name, sh_size))
        elif sh_flags & SHF_EXECINSTR:
            text_b += sh_size
        elif (sh_flags & SHF_ALLOC) and not (sh_flags & SHF_WRITE):
            rodata_b += sh_size

        if sh_type == 2 and sh_entsize == 24:  # SHT_SYMTAB
            sym_str_off = sh_list[sh_link][3]
            for s_off in range(sh_offset, sh_offset + sh_size, 24):
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from('<IBBHQQ', data, s_off)
                if st_name == 0:
                    continue
                end = data.index(b'\x00', sym_str_off + st_name)
                sname = data[sym_str_off + st_name:end].decode('ascii', errors='replace')
                bind = st_info >> 4
                if st_shndx == SHN_COMMON:
                    common_syms.append(sname)
                elif st_shndx == 0 and bind == 1:
                    all_undefs.add(sname)
                elif st_shndx != 0 and bind == 1:
                    all_defs.add(sname)

    # Disassemble and verify 0 FP/SIMD instructions
    dis = subprocess.check_output(['objdump', '-d', str(p)], text=True)
    fp_hits = []
    for line in dis.splitlines():
        if '\t' in line:
            parts = line.split('\t')
            if len(parts) >= 3:
                insn = ' '.join(parts[2:]).split('//', 1)[0].strip()
                if insn and vec_re.search(insn):
                    fp_hits.append(insn)

    assert not writable_secs, f"{p.name}: FORBIDDEN writable sections: {writable_secs}"
    assert not common_syms, f"{p.name}: FORBIDDEN COMMON symbols: {common_syms}"
    assert not fp_hits, f"{p.name}: FORBIDDEN FP/SIMD instructions: {fp_hits[:5]}"

    total_text += text_b
    total_rodata += rodata_b
    print(f"  {p.name:<20} .text={text_b:6d} B  .rodata={rodata_b:4d} B  writable=0 B  COMMON=0  FP/SIMD=0")

ext_undefs = sorted(all_undefs - all_defs)
print(f"  {'TOTAL':<20} .text={total_text:6d} B  .rodata={total_rodata:4d} B  RX+RO={total_text + total_rodata} B ({(total_text+total_rodata)/1024:.2f} KiB)")
print(f"  External imports ({len(ext_undefs)}): {', '.join(ext_undefs)}")
print("ALL AArch64 ELF AUDITS PASSED (0 .data/.bss, 0 COMMON, 0 FP/SIMD)!")
PY
