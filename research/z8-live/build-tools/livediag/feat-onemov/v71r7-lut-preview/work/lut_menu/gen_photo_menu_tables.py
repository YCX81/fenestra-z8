#!/usr/bin/env python3
"""V71.7 Static RO Photo Shooting Menu Table Generator (LUT-Only, count=48).

Adapted from Fenestra-Z8-C311-source host/build_z8_photo_menu.py for static
unified EL1 integration into V71.7 Bank 2 RO (0x40981000..0x40990000).

Key differences vs upstream dynamic WLM capsule:
  - Strictly LUT-only mode (count=48): clones all 47 rows from Stage 0
    `final/stage0-base.main` at 0x43DCF790 / 0x43DCFFA8 (preserving NFP v0.5.1
    Row 45 Pixel Shift 0x2C13 intact!) and appends Row 47 "Select LUT / 选择 LUT"
    (0x2DE7 -> Child Page 0x5D6 at 0xA02917D00, 23 rows 0x4800..0x4816).
  - EXCLUDES BURST row & Page 0x5D5 (0xA02917CA0 remains untouched zero).
  - Emits pure C const-data (`photo_menu_ro_tables.c`) using real link-time C
    symbols (zero hardcoded Bank 2 RO addresses, zero .data/.bss!) + JSON
    receipt (`photo_menu_tables.json`) + exact 2 RAM init patch builders:
      1. 0xA028F5670 (file 0x4C00670, 8 B): parent descriptor ptr -> &v71r7_photo_parent_desc
      2. 0xA02917D00 (file 0x4C22D00, 96 B): child_state(0x5D6, &v71r7_lut_child_vector, &v71r7_lut_child_desc)
  - Audits 100% of zh-CN, zh-TW, and JA strings in `photo_ui.c` against our
    Stage 3 font set (`Font[2]/Font[0]/Font[13] + Font[18] + cjk_ext_1 + cjk_ext_2`),
    requiring 0 missing glyphs!
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from typing import Dict, List, Set, Tuple

HERE = Path(__file__).resolve().parent
V71 = HERE.parents[1]
DEFAULT_STAGE0 = V71 / "final/stage0-base.main"
DEFAULT_UI_C = HERE / "photo_ui.c"
DEFAULT_CJK1 = V71 / "work/font-zh/fonts/cjk_ext_1.hif"
DEFAULT_CJK2 = V71 / "work/font-zh/fonts/cjk_ext_2.hif"
DEFAULT_OUT_C = HERE / "photo_menu_ro_tables.c"
DEFAULT_OUT_JSON = HERE / "photo_menu_tables.json"

# Nikon Z8 C3.11 address space constants
BASE = 0x40160000
INIT_FILE = 0x4B13000
INIT_RAM = 0xA02808000
INIT_BYTES = 0x2AD000

BANK2_LO = 0x40981000
BANK2_HI = 0x40990000

# Stock/Stage0 Photo Shooting Menu addresses & preimages
PAGE_ROOT = 0xA028F4CC0
PARENT_PAGE = 0x19
PARENT_POINTER = 0xA028F5670
PARENT_POINTER_FILE = INIT_FILE + (PARENT_POINTER - INIT_RAM)  # 0x4C00670
PARENT_ORIG = 0xA028CC480
PARENT_ORIG_SHA256 = "95b4499df9384a1ccaa34b3ef91a570450cd4400251d002d8acbc80ecda1cf65"
PARENT_ORIG_LAYOUT = 0x43DCF680
PARENT_ORIG_ROWS = 0x43DCF790
PARENT_ORIG_EVENTS = 0x43DCFFA8
PARENT_TEMPLATE_EVENT = 0x43E286C0
PARENT_ACTIVATIONS = {5: 0x286, 6: 0x28B, 9: 0x29F, 11: 0x2AE}

# Appended Row 47 (LUT-only)
LUT_PARENT_ITEM = 0x2DE7
LUT_TITLE_TEXT = 0x1F42
LUT_HELP_TEXT = 0x1F43
LUT_PAGE = 0x5D6
LUT_STATE = 0xA02917D00
LUT_STATE_FILE = INIT_FILE + (LUT_STATE - INIT_RAM)  # 0x4C22D00
BURST_STATE = 0xA02917CA0

ROW_BYTES = 0x2C
EVENT_BYTES = 0x20
PIXEL_ROW = 45
PIXEL_EVENT = 0x43E27B40
PIXEL_EFFECTIVE_NFP = (
    0xC46, 0xC47, 0xC46, 0, 0x101040, 0xFFFF, 0x2C13, 0x40004,
    0xFFFFFFFF, 0xFFFFFFFF, 2,
)
ROW46_TEMPLATE = (
    0x702, 0xA3C, 0x702, 0, 0x101040, 0xFFFF, 0x2D0D, 0x40004,
    0xFFFFFFFF, 0xFFFFFFFF, 2,
)

CHILD_TEMPLATE = 0xA028F5800
CHILD_EVENTS_TEMPLATE = 0x43BFF8E8
CHILD_EVENTS = (
    0x27E, 0x27F, 0x283, 0x284, 0x285, 0x286, 0x28B, 0x29D,
    0x29E, 0x29F, 0x2A0, 0x2AE, 0x281, 0x306, 0x3BB,
)
LUT_DESC_TEMPLATE_FILE = 0x4BDF810
LUT_LAYOUT_BASE = 0x43DF2608
LUT_ROW_TEMPLATE_VA = 0x43F1F704
LUT_CHILD_COUNT = 23
LUT_VIEWPORT = (6, 60, 714, 418)
LUT_ROW_BASE_ITEM = 0x4800

# Stock font table addresses (for Stage 3 CJK glyph coverage proof)
FONT_PTR_TABLE = 0x42DEC310
FONT_SIZE_TABLE = 0x42DEC3D0


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def unpack(fmt: str, data: bytes, offset: int = 0):
    return struct.unpack_from("<" + fmt, data, offset)


def read_main(data: bytes, address: int, size: int) -> bytes:
    if BASE <= address and address + size <= BASE + len(data):
        off = address - BASE
    elif INIT_RAM <= address and address + size <= INIT_RAM + INIT_BYTES:
        off = INIT_FILE + (address - INIT_RAM)
    else:
        raise ValueError(f"Address out of stage0-base.main range: {address:#x} (+{size})")
    chunk = data[off:off + size]
    require(len(chunk) == size, f"Short read at {address:#x}")
    return chunk


# ============================================================================
# 1. Stage 3 Font Set CJK Glyph Coverage Audit (zh-CN, zh-TW, JA)
# ============================================================================
def parse_hif_cmap(blob: bytes) -> Set[int]:
    require(len(blob) >= 0x46 and blob[:4] == b"hif\x01", "Invalid hif\\x01 font header")
    flags = struct.unpack_from("<H", blob, 6)[0]
    if flags == 6:
        sections = struct.unpack_from("<H", blob, 0x40)[0]
        primary = 0x40 + 6 + 5 * (sections - 1)
    elif flags == 0:
        primary = struct.unpack_from("<I", blob, 0x28)[0]
    else:
        raise ValueError(f"Unsupported hif flags={flags}")
    cmap_cnt = struct.unpack_from("<H", blob, primary)[0]
    cps: Set[int] = set()
    for first, _gid, run in struct.iter_unpack("<3H", blob[primary + 2:primary + 2 + 6 * cmap_cnt]):
        for i in range(run):
            cps.add(first + i)
    return cps


def extract_stock_font(main_bytes: bytes, font_idx: int) -> bytes:
    ptr = struct.unpack_from("<Q", main_bytes, FONT_PTR_TABLE - BASE + font_idx * 8)[0]
    sz = struct.unpack_from("<I", main_bytes, FONT_SIZE_TABLE - BASE + font_idx * 4)[0]
    return read_main(main_bytes, ptr, sz)


def audit_stage3_cjk_glyphs(main_bytes: bytes, ui_source: str,
                            cjk1_path: Path, cjk2_path: Path) -> Dict:
    """Verify 0 missing glyphs across ALL zh-CN, zh-TW, and JA strings in photo_ui.c."""
    f0 = parse_hif_cmap(extract_stock_font(main_bytes, 0))    # zh-TW stock
    f2 = parse_hif_cmap(extract_stock_font(main_bytes, 2))    # zh-CN stock
    f13 = parse_hif_cmap(extract_stock_font(main_bytes, 13))  # JA stock
    f18 = parse_hif_cmap(extract_stock_font(main_bytes, 18))  # ASCII + symbols
    cjk1_raw = cjk1_path.read_bytes()
    cjk2_raw = cjk2_path.read_bytes()
    cjk1 = parse_hif_cmap(cjk1_raw)
    cjk2 = parse_hif_cmap(cjk2_raw)

    sc_charset = f2 | f18 | cjk1 | cjk2
    tc_charset = f0 | f18 | cjk1 | cjk2
    ja_stock_charset = f13 | f18
    ja_stage3_charset = f13 | f18 | cjk1 | cjk2

    # Strip comments and preprocessor directives before parsing LABEL4(...)
    code = re.sub(r"/\*.*?\*/", "", ui_source, flags=re.DOTALL)
    code = re.sub(r"//[^\n]*", "", code)
    code = re.sub(r"^\s*#.*$", "", code, flags=re.MULTILINE)

    lit = r'("(?:[^"\\\n]|\\.)*")'
    pat = re.compile(
        r"\bLABEL4\s*\(\s*" + lit + r"\s*,\s*" + lit + r"\s*,\s*" + lit + r"\s*,\s*" + lit + r"\s*\)"
    )
    quads = pat.findall(code)
    total_calls = len(re.findall(r"\bLABEL4\s*\(", code))
    require(len(quads) == total_calls and len(quads) >= 35,
            f"LABEL4 parse mismatch: parsed {len(quads)} vs {total_calls} occurrences")

    # Catch any non-ASCII literal outside LABEL4()!
    code_no_label4 = pat.sub("", code)
    stray_non_ascii = sorted({
        ch for q in re.findall(lit, code_no_label4) for ch in json.loads(q) if ord(ch) > 127
    })
    require(not stray_non_ascii,
            "Found non-ASCII string literal outside LABEL4(): " +
            ", ".join(f"{c} U+{ord(c):04X}" for c in stray_non_ascii))

    digits = set(" 0123456789%")
    sc_used = {ch for _, sc, _, _ in quads for ch in json.loads(sc)} | digits
    tc_used = {ch for _, _, tc, _ in quads for ch in json.loads(tc)} | digits
    ja_used = {ch for _, _, _, ja in quads for ch in json.loads(ja)} | digits

    sc_missing = sorted(ch for ch in sc_used if ord(ch) not in sc_charset)
    tc_missing = sorted(ch for ch in tc_used if ord(ch) not in tc_charset)
    ja_stock_missing = sorted(ch for ch in ja_used if ord(ch) not in ja_stock_charset)
    ja_stage3_missing = sorted(ch for ch in ja_used if ord(ch) not in ja_stage3_charset)

    require(not sc_missing,
            "zh-CN Stage 3 font set missing: " + ", ".join(f"{c} U+{ord(c):04X}" for c in sc_missing))
    require(not tc_missing,
            "zh-TW Stage 3 font set missing: " + ", ".join(f"{c} U+{ord(c):04X}" for c in tc_missing))
    require(not ja_stock_missing,
            "JA stock Font[13]+Font[18] missing: " + ", ".join(f"{c} U+{ord(c):04X}" for c in ja_stock_missing))

    return {
        "label4_entries": len(quads),
        "fonts": {
            "font0_zh_tw_glyphs": len(f0),
            "font2_zh_cn_glyphs": len(f2),
            "font13_ja_glyphs": len(f13),
            "font18_ascii_glyphs": len(f18),
            "cjk_ext_1_glyphs": len(cjk1),
            "cjk_ext_1_sha256": sha256_hex(cjk1_raw),
            "cjk_ext_2_glyphs": len(cjk2),
            "cjk_ext_2_sha256": sha256_hex(cjk2_raw),
            "zh_cn_union_glyphs": len(sc_charset),
            "zh_tw_union_glyphs": len(tc_charset),
            "ja_stock_union_glyphs": len(ja_stock_charset),
        },
        "zh_cn": {
            "used_chars": len(sc_used),
            "distinct_non_ascii": sum(1 for c in sc_used if ord(c) > 127),
            "missing_count": len(sc_missing),
            "missing": sc_missing,
            "coverage_percent": 100.0,
        },
        "zh_tw": {
            "used_chars": len(tc_used),
            "distinct_non_ascii": sum(1 for c in tc_used if ord(c) > 127),
            "missing_count": len(tc_missing),
            "missing": tc_missing,
            "coverage_percent": 100.0,
        },
        "ja": {
            "used_chars": len(ja_used),
            "distinct_non_ascii": sum(1 for c in ja_used if ord(c) > 127),
            "stock_missing_count": len(ja_stock_missing),
            "stage3_missing_count": len(ja_stage3_missing),
            "missing": ja_stock_missing,
            "coverage_percent": 100.0,
        },
    }


# ============================================================================
# 2. Preimage Verification & Table Extraction From final/stage0-base.main
# ============================================================================
def verify_stage0_preimages(main_bytes: bytes) -> Dict:
    parent_ptr = read_main(main_bytes, PARENT_POINTER, 8)
    require(parent_ptr == struct.pack("<Q", PARENT_ORIG),
            f"PARENT_POINTER preimage mismatch at {PARENT_POINTER:#x}: {parent_ptr.hex()}")

    lut_state = read_main(main_bytes, LUT_STATE, 96)
    require(lut_state == bytes(96), f"LUT_STATE {LUT_STATE:#x} is not zero")

    burst_state = read_main(main_bytes, BURST_STATE, 96)
    require(burst_state == bytes(96), f"BURST_STATE {BURST_STATE:#x} is not zero")

    require(PAGE_ROOT + LUT_PAGE * 96 == LUT_STATE, "LUT page slot mapping differs")

    parent_desc = read_main(main_bytes, PARENT_ORIG, 80)
    require(sha256_hex(parent_desc) == PARENT_ORIG_SHA256, "PARENT_ORIG descriptor SHA-256 differs")
    require(unpack("I", parent_desc, 0)[0] == 0x80001003 and
            unpack("3QH", parent_desc, 8) == (PARENT_ORIG_LAYOUT, PARENT_ORIG_ROWS, PARENT_ORIG_EVENTS, 47),
            "PARENT_ORIG descriptor fields differ")

    bank2_slice = read_main(main_bytes, BANK2_LO, BANK2_HI - BANK2_LO)
    require(not any(bank2_slice), f"Bank 2 cave {BANK2_LO:#x}..{BANK2_HI:#x} is not clean zero!")

    rows47 = read_main(main_bytes, PARENT_ORIG_ROWS, 47 * ROW_BYTES)
    evts47 = read_main(main_bytes, PARENT_ORIG_EVENTS, 47 * 8)

    r45 = unpack("11I", rows47, PIXEL_ROW * ROW_BYTES)
    r46 = unpack("11I", rows47, 46 * ROW_BYTES)
    require(r46 == ROW46_TEMPLATE, f"Row 46 clone template differs: {r46}")
    require(unpack("Q", evts47, PIXEL_ROW * 8)[0] == PIXEL_EVENT, "Row 45 event pointer differs")
    require(unpack("Q", evts47, 46 * 8)[0] == PARENT_TEMPLATE_EVENT, "Row 46 event pointer differs")

    child_tpl = read_main(main_bytes, CHILD_TEMPLATE, 96)
    require(unpack("I", child_tpl, 0)[0] == 0x001E0002 and unpack("Q", child_tpl, 0x50)[0] == 0xA028CC610,
            "CHILD_TEMPLATE 0xA028F5800 differs")

    return {
        "stage0_sha256": sha256_hex(main_bytes),
        "stage0_bytes": len(main_bytes),
        "parent_pointer_va": hex(PARENT_POINTER),
        "parent_pointer_file": hex(PARENT_POINTER_FILE),
        "parent_pointer_preimage": parent_ptr.hex(),
        "parent_orig_sha256": PARENT_ORIG_SHA256,
        "lut_state_va": hex(LUT_STATE),
        "lut_state_file": hex(LUT_STATE_FILE),
        "lut_state_preimage_all_zero": True,
        "burst_state_untouched_zero": True,
        "bank2_cave_zero_verified": True,
        "row45_nfp_pixel_shift_preserved": (r45 == PIXEL_EFFECTIVE_NFP),
        "row45_item_id": hex(r45[6]),
        "cloned_47_rows_sha256": sha256_hex(rows47),
        "cloned_47_events_sha256": sha256_hex(evts47),
    }


def build_parent_event_program(main_bytes: bytes, target_page: int) -> List[Tuple[int, int, int, int]]:
    raw = read_main(main_bytes, PARENT_TEMPLATE_EVENT, 0xBA0)
    entries = list(struct.iter_unpack("<4Q", raw))
    require(len(entries) == 93 and entries[92] == (0x3BB, 0, 0, 0x68C),
            "Parent event template terminal differs")
    out: List[Tuple[int, int, int, int]] = []
    for idx, (ev, cb, guard, tgt) in enumerate(entries):
        if idx in PARENT_ACTIVATIONS:
            expected_ev = PARENT_ACTIVATIONS[idx]
            expected_guard = 0x403B1C64 if idx == 9 else 0
            require((ev, cb, guard, tgt) == (expected_ev, 0x403B1BA8, expected_guard, 0xC6),
                    f"Parent activation event[{idx}] differs: {(ev, cb, guard, tgt)}")
            tgt = target_page
        out.append((ev, cb, guard, tgt))
    return out


def build_lut_child_layout(main_bytes: bytes) -> List[List[int]]:
    def resource(index: int) -> List[int]:
        return list(unpack("17I", read_main(main_bytes, LUT_LAYOUT_BASE + index * 68, 68)))

    r0 = resource(0)
    title, footer, select, status = [resource(i) for i in (1, 7, 8, 5)]
    require(title[14] == 0x2E5 and status[14] == 0x3E7 and select[0] == 0xABE,
            "LUT browser frame resource templates differ")
    # Distinct title and status rectangles in fixed header (exact upstream geometry)
    title[0], title[7], title[14] = LUT_TITLE_TEXT, 340 | (48 << 16), 0x1400
    footer[14] = 0x1402
    select[0], select[14] = 0x1F4A, 0x1403
    back = select.copy()
    back[14] = 0x1404
    status[0], status[14] = 0x1F4A, 0x1405
    status[6:10] = [350 | (8 << 16), 714 | (48 << 16)] * 2
    status[10:14] = [56, 56, 39, 39]
    zero_term = [0] * 17
    return [r0, title, footer, select, back, status, zero_term]


def build_lut_child_rows(main_bytes: bytes) -> List[List[int]]:
    tpl = list(unpack("11I", read_main(main_bytes, LUT_ROW_TEMPLATE_VA, ROW_BYTES)))
    require(tpl[4] == 0x101E00 and tpl[6] == 0x4675, "LUT child row template differs")
    rows: List[List[int]] = []
    for idx in range(LUT_CHILD_COUNT):
        row = tpl.copy()
        row[0] = {0: 0x1F47, 21: 0x1F48, 22: 0x1F49}.get(idx, 0x1F4A)
        row[4] |= 65
        row[6] = LUT_ROW_BASE_ITEM + idx
        rows.append(row)
    return rows


def build_lut_child_events(main_bytes: bytes) -> List[Tuple[int, str, int, int]]:
    raw = read_main(main_bytes, CHILD_EVENTS_TEMPLATE, 15 * EVENT_BYTES)
    transitions = list(struct.iter_unpack("<4Q", raw))
    require(tuple(r[0] for r in transitions) == CHILD_EVENTS, "Child event template differs")
    out: List[Tuple[int, str, int, int]] = []
    for ev, cb, guard, tgt in transitions:
        cb_expr = f"(PhotoMenuCallback)(uintptr_t)UINT64_C({cb:#x})" if cb else "NULL"
        if ev in PARENT_ACTIVATIONS.values():
            require(cb == 0x403B1BA8 and guard == (0x403B1C64 if ev == 0x29F else 0),
                    "Child activate ABI changed")
            cb_expr = "photo_lut_activate"
            tgt = 0x68C
        elif ev in (0x285, 0x2A0, 0x306):
            require(tgt == 0x68D and cb == (0 if ev == 0x306 else 0x403B1BA4)
                    and guard == (0x403B1C64 if ev == 0x2A0 else 0),
                    "Child back template differs")
            cb_expr = "photo_lut_back_action"
            tgt = PARENT_PAGE
        out.append((ev, cb_expr, guard, tgt))
    return out


def build_lut_child_state_template(main_bytes: bytes) -> bytes:
    """96-byte child_state(0x5D6) base template; Stage 1 patches +0x28 (vector VA) & +0x50 (desc VA)."""
    state = bytearray(read_main(main_bytes, CHILD_TEMPLATE, 96))
    struct.pack_into("<I", state, 0x00, (LUT_PAGE << 16) | 2)
    struct.pack_into("<Q", state, 0x28, 0)
    struct.pack_into("<Q", state, 0x50, 0)
    return bytes(state)


def build_ram_init_patches(main_bytes: bytes, parent_desc_va: int,
                           lut_vector_va: int, lut_desc_va: int) -> List[Dict]:
    """Build the exact 2 RAM init patches for Stage 1 once Bank 2 ELF symbols are linked."""
    require(BANK2_LO <= parent_desc_va < BANK2_HI and (parent_desc_va & 7) == 0,
            f"v71r7_photo_parent_desc VA out of Bank 2: {parent_desc_va:#x}")
    require(BANK2_LO <= lut_vector_va < BANK2_HI and (lut_vector_va & 7) == 0,
            f"v71r7_lut_child_vector VA out of Bank 2: {lut_vector_va:#x}")
    require(BANK2_LO <= lut_desc_va < BANK2_HI and (lut_desc_va & 7) == 0,
            f"v71r7_lut_child_desc VA out of Bank 2: {lut_desc_va:#x}")

    p1_old = read_main(main_bytes, PARENT_POINTER, 8)
    require(p1_old == struct.pack("<Q", PARENT_ORIG), "PARENT_POINTER preimage mismatch")
    p1_new = struct.pack("<Q", parent_desc_va)

    p2_old = read_main(main_bytes, LUT_STATE, 96)
    require(p2_old == bytes(96), "LUT_STATE preimage mismatch")
    state = bytearray(build_lut_child_state_template(main_bytes))
    struct.pack_into("<Q", state, 0x28, lut_vector_va)
    struct.pack_into("<Q", state, 0x50, lut_desc_va)
    p2_new = bytes(state)

    return [
        {
            "name": "photo_shooting_menu_parent_desc_ptr",
            "va": hex(PARENT_POINTER),
            "file_offset": hex(PARENT_POINTER_FILE),
            "bytes": 8,
            "symbol": "v71r7_photo_parent_desc",
            "old_hex": p1_old.hex(),
            "new_hex": p1_new.hex(),
        },
        {
            "name": "photo_lut_child_page_0x5d6_state",
            "va": hex(LUT_STATE),
            "file_offset": hex(LUT_STATE_FILE),
            "bytes": 96,
            "symbols": {
                "offset_0x28": "v71r7_lut_child_vector",
                "offset_0x50": "v71r7_lut_child_desc",
            },
            "old_hex": p2_old.hex(),
            "new_hex": p2_new.hex(),
        },
    ]


# ============================================================================
# 3. Pure Const C Emitter (photo_menu_ro_tables.c)
# ============================================================================
def fmt_bytes(b: bytes, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(b), 12):
        lines.append(indent + ", ".join(f"0x{x:02x}u" for x in b[i:i + 12]))
    return ",\n".join(lines)


def generate_c_source(main_bytes: bytes) -> Tuple[str, Dict]:
    parent_orig = read_main(main_bytes, PARENT_ORIG, 80)
    parent_vp = unpack("4h", parent_orig, 0x22)
    parent_tail = parent_orig[0x2A:0x50]
    require(len(parent_tail) == 38, "Parent descriptor tail length")

    rows47_raw = read_main(main_bytes, PARENT_ORIG_ROWS, 47 * ROW_BYTES)
    evts47_raw = read_main(main_bytes, PARENT_ORIG_EVENTS, 47 * 8)

    parent_rows: List[List[int]] = [
        list(unpack("11I", rows47_raw, i * ROW_BYTES)) for i in range(47)
    ]
    # Append Row 47: Select LUT (0x2DE7 -> Page 0x5D6)
    row47 = list(ROW46_TEMPLATE)
    row47[0] = LUT_TITLE_TEXT
    row47[1] = LUT_HELP_TEXT
    row47[2] = LUT_TITLE_TEXT
    row47[6] = LUT_PARENT_ITEM
    parent_rows.append(row47)
    require(len(parent_rows) == 48, "Expected 48 parent rows")

    parent_events_47 = list(unpack("47Q", evts47_raw, 0))
    parent_lut_prog = build_parent_event_program(main_bytes, LUT_PAGE)

    lut_desc_raw = main_bytes[LUT_DESC_TEMPLATE_FILE:LUT_DESC_TEMPLATE_FILE + 80]
    require(len(lut_desc_raw) == 80, "LUT descriptor template length")
    lut_desc_pad0 = unpack("I", lut_desc_raw, 4)[0]
    lut_desc_tail = lut_desc_raw[0x2A:0x50]
    require(len(lut_desc_tail) == 38, "LUT descriptor tail length")

    lut_layout_rows = build_lut_child_layout(main_bytes)
    lut_child_rows = build_lut_child_rows(main_bytes)
    lut_child_evts = build_lut_child_events(main_bytes)

    lines: List[str] = [
        "/* SPDX-License-Identifier: GPL-3.0-only */",
        "/* AUTO-GENERATED by work/lut_menu/gen_photo_menu_tables.py from final/stage0-base.main.",
        " * DO NOT EDIT BY HAND. Pure const .rodata tables for V71.7 Bank 2 (0x40981000..0x40990000).",
        " * Mode: LUT-only (count=48, Row 47 = 0x2DE7 Select LUT -> Child Page 0x5D6, BURST excluded).",
        " * ZERO .data, ZERO .bss, STRICT SCALAR.",
        " */",
        '#include "photo_menu.h"',
        "",
        "/* 48 Photo Shooting Menu rows (0..46 cloned from stage0-base.main + Row 47 Select LUT) */",
        "const V71r7MenuRow v71r7_photo_parent_rows[PHOTO_PARENT_ROW_COUNT] = {",
    ]
    for idx, words in enumerate(parent_rows):
        wstr = ", ".join(f"0x{w:08x}u" for w in words)
        tag = " /* Row 47: Select LUT (0x2DE7) */" if idx == 47 else (
              " /* Row 45: NFP Pixel Shift (0x2C13) */" if idx == 45 else "")
        lines.append(f"    {{ {{ {wstr} }} }},{tag}")
    lines.append("};")
    lines.append("")

    lines.append("/* Row 47 Select LUT activation event program (0xBA0 = 93 x 32 B -> target Page 0x5D6) */")
    lines.append("const V71r7MenuEventEntry v71r7_photo_parent_lut_event_prog[93] = {")
    for idx, (ev, cb, guard, tgt) in enumerate(parent_lut_prog):
        cb_s = f"(PhotoMenuCallback)(uintptr_t)UINT64_C({cb:#x})" if cb else "NULL"
        lines.append(f"    {{ UINT64_C({ev:#x}), {cb_s}, UINT64_C({guard:#x}), UINT64_C({tgt:#x}) }},")
    lines.append("};")
    lines.append("")

    lines.append("/* 48 Photo Shooting Menu event table pointers (0..46 cloned + 47 -> LUT program) */")
    lines.append("const void *const v71r7_photo_parent_events[PHOTO_PARENT_ROW_COUNT] = {")
    for idx, ptr in enumerate(parent_events_47):
        lines.append(f"    (const void *)(uintptr_t)UINT64_C({ptr:#x}), /* row {idx} */")
    lines.append("    &v71r7_photo_parent_lut_event_prog[0] /* row 47: Select LUT */")
    lines.append("};")
    lines.append("")

    lines.append("/* Cloned Photo Shooting Menu parent descriptor (count = 48) */")
    lines.append("const V71r7MenuDescriptor v71r7_photo_parent_desc = {")
    lines.append("    .flags    = 0x80001003u,")
    lines.append(f"    .pad0     = {unpack('I', parent_orig, 4)[0]:#x}u,")
    lines.append(f"    .layout   = (const void *)(uintptr_t)UINT64_C({PARENT_ORIG_LAYOUT:#x}),")
    lines.append("    .rows     = &v71r7_photo_parent_rows[0],")
    lines.append("    .events   = &v71r7_photo_parent_events[0],")
    lines.append("    .count    = PHOTO_PARENT_ROW_COUNT,")
    lines.append(f"    .viewport = {{ {parent_vp[0]}, {parent_vp[1]}, {parent_vp[2]}, {parent_vp[3]} }},")
    lines.append("    .tail     = {")
    lines.append(fmt_bytes(parent_tail, "        "))
    lines.append("    }")
    lines.append("};")
    lines.append("")

    lines.append("/* Child Page 0x5D6 header/footer/status widget layout (7 x 68 B = 476 B) */")
    lines.append("const uint32_t v71r7_lut_child_layout[7][17] = {")
    for r in lut_layout_rows:
        lines.append("    { " + ", ".join(f"0x{w:08x}u" for w in r) + " },")
    lines.append("};")
    lines.append("")

    lines.append("/* Child Page 0x5D6 rows (23 x 44 B = 1012 B: 0x4800..0x4816) */")
    lines.append("const V71r7MenuRow v71r7_lut_child_rows[PHOTO_LUT_ROWS] = {")
    for idx, words in enumerate(lut_child_rows):
        lines.append("    { { " + ", ".join(f"0x{w:08x}u" for w in words) + f" }} }}, /* 0x{LUT_ROW_BASE_ITEM + idx:04x} */")
    lines.append("};")
    lines.append("")

    lines.append("/* Child Page 0x5D6 navigation & activation event table (15 x 32 B = 480 B) */")
    lines.append("const V71r7MenuEventEntry v71r7_lut_child_event_table[15] = {")
    for ev, cb_expr, guard, tgt in lut_child_evts:
        lines.append(f"    {{ UINT64_C({ev:#x}), {cb_expr}, UINT64_C({guard:#x}), UINT64_C({tgt:#x}) }},")
    lines.append("};")
    lines.append("")

    lines.append("/* Child Page 0x5D6 per-row event table pointers (23 x 8 B = 184 B) */")
    lines.append("const void *const v71r7_lut_child_events[PHOTO_LUT_ROWS] = {")
    for _ in range(LUT_CHILD_COUNT):
        lines.append("    &v71r7_lut_child_event_table[0],")
    lines.append("};")
    lines.append("")

    lines.append("/* Child Page 0x5D6 lifecycle callback vector (5 x 8 B = 40 B) */")
    lines.append("const PhotoMenuCallback v71r7_lut_child_vector[5] = {")
    lines.append("    photo_lut_entry,")
    lines.append("    photo_lut_exit,")
    lines.append("    (PhotoMenuCallback)(uintptr_t)UINT64_C(0x403b2210),")
    lines.append("    NULL,")
    lines.append("    photo_lut_refresh")
    lines.append("};")
    lines.append("")

    lines.append("/* Child Page 0x5D6 descriptor (80 B, count = 23, viewport {6, 60, 714, 418}) */")
    lines.append("const V71r7MenuDescriptor v71r7_lut_child_desc = {")
    lines.append("    .flags    = 0x80001818u,")
    lines.append(f"    .pad0     = {lut_desc_pad0:#x}u,")
    lines.append("    .layout   = &v71r7_lut_child_layout[0][0],")
    lines.append("    .rows     = &v71r7_lut_child_rows[0],")
    lines.append("    .events   = &v71r7_lut_child_events[0],")
    lines.append("    .count    = PHOTO_LUT_ROWS,")
    lines.append(f"    .viewport = {{ {LUT_VIEWPORT[0]}, {LUT_VIEWPORT[1]}, {LUT_VIEWPORT[2]}, {LUT_VIEWPORT[3]} }},")
    lines.append("    .tail     = {")
    lines.append(fmt_bytes(lut_desc_tail, "        "))
    lines.append("    }")
    lines.append("};")
    lines.append("")

    sizes = {
        "v71r7_photo_parent_desc": 80,
        "v71r7_photo_parent_rows": 48 * ROW_BYTES,
        "v71r7_photo_parent_events": 48 * 8,
        "v71r7_photo_parent_lut_event_prog": 93 * EVENT_BYTES,
        "v71r7_lut_child_desc": 80,
        "v71r7_lut_child_vector": 5 * 8,
        "v71r7_lut_child_layout": 7 * 17 * 4,
        "v71r7_lut_child_rows": LUT_CHILD_COUNT * ROW_BYTES,
        "v71r7_lut_child_events": LUT_CHILD_COUNT * 8,
        "v71r7_lut_child_event_table": 15 * EVENT_BYTES,
    }
    sizes["total_ro_table_bytes"] = sum(sizes.values())
    return "\n".join(lines), sizes


def generate(stage0_path: Path, ui_c_path: Path, cjk1_path: Path, cjk2_path: Path,
             out_c: Path, out_json: Path) -> Dict:
    main_bytes = stage0_path.read_bytes()
    preimages = verify_stage0_preimages(main_bytes)
    glyph_audit = audit_stage3_cjk_glyphs(
        main_bytes, ui_c_path.read_text(encoding="utf-8"), cjk1_path, cjk2_path
    )
    c_source, table_sizes = generate_c_source(main_bytes)
    child_state_tpl = build_lut_child_state_template(main_bytes)

    out_c.write_text(c_source, encoding="utf-8")

    # Example preview RAM patches using symbolic placeholder offsets inside Bank 2 RO
    # (Stage 1 calls build_ram_init_patches() with actual linked ELF symbol VAs!)
    sample_patches = build_ram_init_patches(
        main_bytes,
        parent_desc_va=0x4098C000,
        lut_vector_va=0x4098DC00,
        lut_desc_va=0x4098DC40,
    )

    receipt = {
        "schema": "v71r7.photo_menu_tables/v1",
        "mode": "lut_only_count48_no_burst",
        "bank2_window": {"start": hex(BANK2_LO), "end": hex(BANK2_HI), "budget_bytes": BANK2_HI - BANK2_LO},
        "preimages": preimages,
        "table_sizes_bytes": table_sizes,
        "generated_c": {
            "path": str(out_c),
            "bytes": len(c_source.encode("utf-8")),
            "sha256": sha256_hex(c_source.encode("utf-8")),
        },
        "ram_init_patch_spec": {
            "count": 2,
            "patch_1_parent_pointer": {
                "va": hex(PARENT_POINTER),
                "file_offset": hex(PARENT_POINTER_FILE),
                "bytes": 8,
                "preimage_hex": struct.pack("<Q", PARENT_ORIG).hex(),
                "value_symbol": "v71r7_photo_parent_desc",
            },
            "patch_2_lut_child_state": {
                "va": hex(LUT_STATE),
                "file_offset": hex(LUT_STATE_FILE),
                "bytes": 96,
                "page_id": hex(LUT_PAGE),
                "preimage_hex": bytes(96).hex(),
                "template_hex": child_state_tpl.hex(),
                "patch_u64_offset_0x28_symbol": "v71r7_lut_child_vector",
                "patch_u64_offset_0x50_symbol": "v71r7_lut_child_desc",
            },
            "example_bank2_patches": sample_patches,
        },
        "cjk_glyph_audit": glyph_audit,
    }
    out_json.write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return receipt


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stage0", type=Path, default=DEFAULT_STAGE0)
    ap.add_argument("--ui-c", type=Path, default=DEFAULT_UI_C)
    ap.add_argument("--cjk1", type=Path, default=DEFAULT_CJK1)
    ap.add_argument("--cjk2", type=Path, default=DEFAULT_CJK2)
    ap.add_argument("--out-c", type=Path, default=DEFAULT_OUT_C)
    ap.add_argument("--out-json", type=Path, default=DEFAULT_OUT_JSON)
    args = ap.parse_args()

    receipt = generate(args.stage0, args.ui_c, args.cjk1, args.cjk2, args.out_c, args.out_json)
    print(json.dumps({
        "mode": receipt["mode"],
        "table_sizes_bytes": receipt["table_sizes_bytes"],
        "row45_nfp_pixel_shift_preserved": receipt["preimages"]["row45_nfp_pixel_shift_preserved"],
        "bank2_cave_zero_verified": receipt["preimages"]["bank2_cave_zero_verified"],
        "zh_cn_missing": receipt["cjk_glyph_audit"]["zh_cn"]["missing_count"],
        "zh_tw_missing": receipt["cjk_glyph_audit"]["zh_tw"]["missing_count"],
        "ja_stock_missing": receipt["cjk_glyph_audit"]["ja"]["stock_missing_count"],
        "generated_c_sha256": receipt["generated_c"]["sha256"],
    }, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
