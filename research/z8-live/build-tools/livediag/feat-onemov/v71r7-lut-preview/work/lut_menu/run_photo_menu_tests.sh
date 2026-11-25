#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
CORE_DIR="$(cd "$DIR/../lut_core" && pwd)"
OUT_TMP="$(mktemp -d /tmp/v71r7_photo_menu_test.XXXXXX)"
trap 'rm -rf "$OUT_TMP"' EXIT

echo "=== [1/3] Running gen_photo_menu_tables.py (stage0-base.main preimages + Stage 3 CJK glyph audit) ==="
python3 "$DIR/gen_photo_menu_tables.py"

echo ""
echo "=== [2/3] Building & Running Mac Host ASan+UBSan Test Suite ==="
clang -std=c11 -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wall -Wextra -Werror \
  -DV71R7_HOST_TEST=1 \
  -I"$CORE_DIR" -I"$DIR" \
  "$DIR/photo_catalog.c" \
  "$DIR/photo_journal.c" \
  "$DIR/photo_ui.c" \
  "$DIR/photo_worker.c" \
  "$DIR/photo_menu_ro_tables.c" \
  "$DIR/test_photo_menu_host.c" \
  -o "$OUT_TMP/test_photo_menu_host"

"$OUT_TMP/test_photo_menu_host"

echo ""
echo "=== [3/3] AArch64 EL1 Cross-Compile Audit (zero .data/.bss, zero COMMON, zero FP/SIMD) ==="
python3 - "$DIR" "$CORE_DIR" "$OUT_TMP" <<'PY'
import struct, subprocess, sys
from pathlib import Path

menu = Path(sys.argv[1])
core = Path(sys.argv[2])
out  = Path(sys.argv[3])

units = [
    "photo_catalog.c",
    "photo_journal.c",
    "photo_ui.c",
    "photo_worker.c",
    "photo_menu_ro_tables.c",
]
flags = [
    "--target=aarch64-none-elf", "-Oz", "-std=c11",
    "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-stack-protector",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections",
    "-mgeneral-regs-only", "-mstrict-align", "-mno-outline-atomics",
    "-Wall", "-Wextra", "-Werror",
    "-I", str(core), "-I", str(menu),
]

allowed_ext = {
    "memcpy", "memset", "memcmp", "memmove",
    "v71r7_flight_init", "v71r7_flight_record",
    "v71r7_heartbeat_enter", "v71r7_heartbeat_progress", "v71r7_heartbeat_idle",
    "v71r7_watchdog_poll",
    "v71r7_neon_sentinel_is_allowed",
    "v71r7_smp_online_helpers",
    "v71r7_lut_prepare_from_buffer", "v71r7_lut_clear_active", "v71r7_lut_is_active",
    "v71r7_lut_stream_begin", "v71r7_lut_stream_feed",
    "v71r7_lut_stream_commit", "v71r7_lut_stream_abort",
    "v71r7_lut_get_active_path", "v71r7_lut_runtime_get_stats",
    "v71r7_layer0_init", "v71r7_layer0_notify_lut_changed",
    "v71r7_layer0_cache_invalidate_before_swap",
    "v71r7_layer0_worker_step", "v71r7_layer0_workspace_allocated",
    "v71r7_layer0_metrics_ptr", "v71r7_layer0_pool_suspend", "v71r7_layer0_pool_resume",
}

total_text = 0
total_ro = 0
defined, undef = set(), set()

for u in units:
    obj = out / (Path(u).stem + ".o")
    subprocess.run(["clang", *flags, "-c", str(menu / u), "-o", str(obj)], check=True)
    d = obj.read_bytes()
    shoff = struct.unpack_from("<Q", d, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 58)
    str_off = struct.unpack_from("<Q", d, shoff + shstrndx * shentsize + 24)[0]

    t_sz = r_sz = w_sz = fp_simd = 0
    writable = []
    common_syms = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name_i, typ, flg, addr, foff, sz, link, info, align, entsize = struct.unpack_from("<IIQQQQIIQQ", d, off)
        name = d[str_off + name_i:d.index(0, str_off + name_i)].decode()
        is_alloc = bool(flg & 2)
        is_write = bool(flg & 1)
        is_exec  = bool(flg & 4)
        if is_alloc and is_write and sz > 0:
            w_sz += sz
            writable.append((name, sz))
        elif is_alloc and is_exec:
            t_sz += sz
            for p in range(foff, foff + sz, 4):
                w = struct.unpack_from("<I", d, p)[0]
                op0 = (w >> 25) & 0xF
                if op0 in (0x7, 0xF) or ((op0 & 0x5) == 0x4 and ((w >> 26) & 1)):
                    fp_simd += 1
        elif is_alloc:
            r_sz += sz
        if typ == 2:
            s_str = struct.unpack_from("<Q", d, shoff + link * shentsize + 24)[0]
            for s in range(0, sz, entsize):
                st_name, st_info, st_other, st_shndx, st_val, st_size = struct.unpack_from("<IBBHQQ", d, foff + s)
                sym = d[s_str + st_name:d.index(0, s_str + st_name)].decode()
                if st_shndx == 0xFFF2:
                    common_syms.append(sym)
                if (st_info >> 4) != 0:
                    if st_shndx == 0: undef.add(sym)
                    else: defined.add(sym)

    assert w_sz == 0 and not writable, f"{u}: writable sections forbidden: {writable}"
    assert not common_syms, f"{u}: COMMON symbols forbidden: {common_syms}"
    assert fp_simd == 0, f"{u}: FP/SIMD instructions forbidden: {fp_simd}"
    total_text += t_sz
    total_ro += r_sz
    print(f"  [OK] {u:26s} .text={t_sz:5d} B  .rodata={r_sz:5d} B  writable=0 B  COMMON=0  fp/simd=0")

ext = undef - defined
unexpected = sorted(ext - allowed_ext)
assert not unexpected, f"Unexpected external symbols: {unexpected}"
bank2_total = total_text + total_ro
assert bank2_total <= 61440, f"Bank 2 overflow: {bank2_total} > 61440"
print(f"  [OK] Bank 2 total RX+RO: {bank2_total} B (.text={total_text} B + .rodata={total_ro} B), headroom={61440 - bank2_total} B")
print(f"  [OK] External imports ({len(ext)}): {', '.join(sorted(ext))}")
PY
echo ""
echo "=== ALL V71.7 PHOTO MENU VERIFICATION GATES PASSED ==="
