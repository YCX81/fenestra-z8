#!/usr/bin/env python3
"""Comprehensive Unicorn ARM64 verification of V68/V69 FontCJK-ZhUI MAIN against Nikon's real font engine code!

Executes Nikon's ACTUAL ARM64 instructions from candidate MAIN:
  - 0x40881888 (open_font)
  - 0x40880d74 (set_primary_font -> v68_set_primary_font)
  - 0x40883e1c (4-slot cmap glyph lookup)
  - 0x408811d4 (4-slot combined metrics)
  - 0x40881cb4 (load & decode glyph outline — exhaustive 3,603/3,603 CJK_EXT_1 + CJK_EXT_2!)
  - 0x401fe200 (slots.state ops 13/14/15/16)
  - 0x4032e130 (menu_text -> v68_menu_text with dynamically resolved ORIG_TEXT_VA,
    covering Live/PB/Fenestra/Portrait/Slotcard/FilmSim + NFP/stock pass-through + ABI conservation!)
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL, UC_HOOK_CODE
from unicorn.arm64_const import *

BASE = 0x40160000
END = 0x7FFFF000
ALLOC_STUB = 0x7FFF0000
FREE_STUB = 0x7FFF0010
REALLOC_STUB = 0x7FFF0020

LIVE_PRIMARY_LO = 0x401E2000
LIVE_PRIMARY_HI = 0x401FCD00
MENU_TEXT_HOOK_VA = 0x4032E130

HIF_VTABLE = 0x44B82608
ENGINE_VA = 0xA20000000
HEAP_BASE = 0xA21000000
STACK_TOP = 0xA30000000
SCRATCH = 0xA28000000
FLAG_PAGE = 0xA116D3000
LANG_PAGE = 0xA02F2A000
LANG_VA = 0xA02F2AB70
FONT_CACHE_VA = 0xA116D3490
PORTRAIT_FLAG = 0xA116D3480

FONT_PTR_TABLE = 0x42DEC310
FONT_SIZE_TABLE = 0x42DEC3D0

CJK1_VA = 0x408C6000
CJK2_VA = 0x40B8A000


def r32(u, a): return struct.unpack('<I', u.mem_read(a, 4))[0]
def r64(u, a): return struct.unpack('<Q', u.mem_read(a, 8))[0]
def w32(u, a, v): u.mem_write(a, struct.pack('<I', v))
def w64(u, a, v): u.mem_write(a, struct.pack('<Q', v))


def decode_b(at: int, word: int) -> int:
    assert (word >> 26) == 5, f"Expected AArch64 B at {hex(at)}, got 0x{word:08x}"
    imm = word & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    return at + imm * 4


def hif_glyph_count(buf: bytes, va: int) -> int:
    off = va - BASE
    assert buf[off:off + 4] == b'hif\x01', f"Not hif\\x01 at {hex(va)}"
    idx_off = struct.unpack_from('<I', buf, off + 0x2C)[0]
    nr = struct.unpack_from('<H', buf, off + idx_off)[0]
    pos = off + idx_off + 2
    max_gid = 0
    for _ in range(nr):
        first, _base, cnt = struct.unpack_from('<HIH', buf, pos)
        pos += 8 + cnt * 2
        if first + cnt - 1 > max_gid:
            max_gid = first + cnt - 1
    return max_gid


class FontEngineEmu:
    def __init__(self, img_bytes):
        self.img = img_bytes
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        size = (len(img_bytes) + 0xFFF) & ~0xFFF
        u.mem_map(BASE, size, UC_PROT_ALL)
        u.mem_write(BASE, img_bytes)
        # Map initialized .data + BSS just like Nikon boot (0x4B13000..0x4DC0000 -> 0xA02808000)
        u.mem_map(0xA02800000, 0x2600000, UC_PROT_ALL)
        u.mem_write(0xA02808000, img_bytes[0x4B13000:0x4DC0000])
        u.mem_map(0x7FFF0000, 0x10000, UC_PROT_ALL)
        u.mem_map(ENGINE_VA, 0x10000, UC_PROT_ALL)
        u.mem_map(HEAP_BASE, 0x800000, UC_PROT_ALL)  # 8 MiB bump heap
        u.mem_map(STACK_TOP - 0x20000, 0x20000, UC_PROT_ALL)
        u.mem_map(SCRATCH, 0x10000, UC_PROT_ALL)
        u.mem_map(FLAG_PAGE, 0x2000, UC_PROT_ALL)

        u.mem_write(ALLOC_STUB, struct.pack('<I', 0xD65F03C0))
        u.mem_write(FREE_STUB, struct.pack('<I', 0xD65F03C0))
        u.mem_write(REALLOC_STUB, struct.pack('<I', 0xD65F03C0))
        u.mem_write(END, struct.pack('<I', 0xD503201F))
        u.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)

        self.heap_ptr = HEAP_BASE
        self.allocs = 0

        def on_code(uc, addr, sz, _):
            if addr == ALLOC_STUB:
                n = uc.reg_read(UC_ARM64_REG_W0)
                p = self.heap_ptr
                self.heap_ptr = (self.heap_ptr + max(n, 16) + 15) & ~15
                self.allocs += 1
                uc.reg_write(UC_ARM64_REG_X0, p)
            elif addr == FREE_STUB:
                pass
            elif addr == REALLOC_STUB:
                oldp = uc.reg_read(UC_ARM64_REG_X0)
                n = uc.reg_read(UC_ARM64_REG_W1)
                p = self.heap_ptr
                self.heap_ptr = (self.heap_ptr + max(n, 16) + 15) & ~15
                if oldp and oldp >= HEAP_BASE:
                    uc.mem_write(p, bytes(uc.mem_read(oldp, min(n, 65536))))
                uc.reg_write(UC_ARM64_REG_X0, p)

        u.hook_add(UC_HOOK_CODE, on_code, begin=0x7FFF0000, end=0x7FFF0030)
        self.u = u

        # Stub RTOS mutex create (0x40253998: mov x0, #1; ret)
        u.mem_write(0x40887420 - 6503048, struct.pack('<II', 0xD2800020, 0xD65F03C0))

        # Copy Nikon's real callbacks struct from 0x41f1bb10 (96 B), substituting our bump allocator!
        cbs = SCRATCH + 0x800
        u.mem_write(cbs, img_bytes[0x41F1BB10 - BASE:0x41F1BB10 - BASE + 0x60])
        w64(u, cbs + 0, ALLOC_STUB)
        w64(u, cbs + 8, FREE_STUB)
        w64(u, cbs + 16, REALLOC_STUB)
        self.engine = self.call(0x4088070C, cbs, 0x41F1BB70)
        assert self.engine != 0, "Nikon create_engine (0x4088070c) failed!"
        vt_hif = self.call(0x402EDD4C + 5851924)
        assert (self.call(0x402EDD58 + 5844044, self.engine, vt_hif) & 0xFFFFFFFF) == 0
        vt_out = self.call(0x402EDD64 + 5868604)
        assert (self.call(0x402EDD70 + 5844020, self.engine, vt_out) & 0xFFFFFFFF) == 0

    def call(self, fn, x0=0, x1=0, x2=0, x3=0, max_ins=500000):
        u = self.u
        u.reg_write(UC_ARM64_REG_SP, STACK_TOP - 0x100)
        u.reg_write(UC_ARM64_REG_X30, END)
        u.reg_write(UC_ARM64_REG_X0, x0)
        u.reg_write(UC_ARM64_REG_X1, x1)
        u.reg_write(UC_ARM64_REG_X2, x2)
        u.reg_write(UC_ARM64_REG_X3, x3)
        u.emu_start(fn, END, count=max_ins)
        assert u.reg_read(UC_ARM64_REG_PC) == END, f"Did not reach END from {hex(fn)}, pc={hex(u.reg_read(UC_ARM64_REG_PC))}"
        return u.reg_read(UC_ARM64_REG_X0)

    def stock_font_info(self, idx):
        ptr = struct.unpack_from('<Q', self.img, FONT_PTR_TABLE - BASE + idx * 8)[0]
        sz = struct.unpack_from('<I', self.img, FONT_SIZE_TABLE - BASE + idx * 4)[0]
        return ptr, sz

    def open_font(self, ptr, sz):
        return self.call(0x40881888, self.engine, 0, ptr, sz)

    def set_primary(self, face):
        return self.call(0x40880D74, self.engine, face) & 0xFFFFFFFF

    def slots(self):
        return [r64(self.u, self.engine + 232 + i * 8) for i in range(4)]

    def metrics(self):
        out = SCRATCH
        self.u.mem_write(out, b'\x00' * 32)
        rc = self.call(0x408811D4, self.engine, out) & 0xFFFFFFFF
        vals = struct.unpack('<4i', self.u.mem_read(out, 16))
        return rc, vals

    def lookup_cp_in_face(self, face, cp):
        out_gid = SCRATCH + 0x100
        w32(self.u, out_gid, 0xDEAD)
        rc = self.call(0x40883E1C, face, cp, out_gid) & 0xFFFFFFFF
        return rc, r32(self.u, out_gid)

    def lookup_4slot(self, cp):
        for s_idx, face in enumerate(self.slots()):
            if not face:
                continue
            rc, gid = self.lookup_cp_in_face(face, cp)
            if rc == 0 and gid != 0:
                return s_idx, face, gid
        return None, 0, 0

    def decode_glyph_in_face(self, face, gid):
        out_a = SCRATCH + 0x200
        out_b = SCRATCH + 0x220
        rc = self.call(0x40881CB4, face, gid, out_a, out_b) & 0xFFFFFFFF
        return rc


def test_font_engine(cand_img, base_img, exhaustive=True):
    print("=== [1] Testing real Nikon ARM64 font engine (Candidate vs Base) ===")
    ec = FontEngineEmu(cand_img)
    eb = FontEngineEmu(base_img)

    faces_c = {}
    faces_b = {}
    for idx in (0, 2, 13, 16, 18, 22):
        ptr, sz = ec.stock_font_info(idx)
        fc = ec.open_font(ptr, sz)
        fb = eb.open_font(ptr, sz)
        assert fc != 0 and fb != 0, f"open_font failed for Font[{idx}]"
        faces_c[idx] = fc
        faces_b[idx] = fb

    # Verify set_primary_font for Font[2] (zh-CN) binds all 4 slots!
    assert ec.set_primary(faces_c[2]) == 0
    s_zh = ec.slots()
    print(f"  Font[2] slots: {[hex(x) for x in s_zh]}")
    assert s_zh[0] == faces_c[2]
    assert all(x != 0 for x in s_zh), "Expected all 4 slots populated for Font[2]!"
    assert r64(ec.u, s_zh[1] + 24) == 0x42DBF97D  # Font[18]
    assert r64(ec.u, s_zh[2] + 24) == CJK1_VA     # CJK_EXT_1
    assert r64(ec.u, s_zh[3] + 24) == CJK2_VA     # CJK_EXT_2
    for i, f in enumerate(s_zh):
        wccw = r32(ec.u, f + 56)
        assert wccw == 0, f"Slot {i} winding_is_ccw={wccw}, expected 0"

    assert eb.set_primary(faces_b[2]) == 0
    rc_b, m_b = eb.metrics()
    rc_c, m_c = ec.metrics()
    print(f"  Metrics Font[2]: base={m_b} (rc={rc_b}), cand 4-slot={m_c} (rc={rc_c})")
    assert rc_b == 0 and rc_c == 0 and m_b == m_c, f"Metric mismatch! base={m_b} vs cand={m_c}"

    # Verify ALL 95/95 printable ASCII chars under zh-CN, zh-TW, ja, ko!
    for fidx, fname in [(2, 'zh-CN'), (0, 'zh-TW'), (13, 'ja'), (22, 'ko')]:
        assert ec.set_primary(faces_c[fidx]) == 0
        assert eb.set_primary(faces_b[fidx]) == 0
        _, mbm = eb.metrics()
        _, mcm = ec.metrics()
        assert mbm == mcm, f"Metrics mismatch for {fname}: {mbm} vs {mcm}"
        slot_hits = [0, 0, 0, 0]
        for cp in range(0x20, 0x7F):
            sl, f, gid = ec.lookup_4slot(cp)
            assert sl in (0, 1) and gid != 0, f"{fname} missing ASCII U+{cp:04X} ({chr(cp)!r})"
            slot_hits[sl] += 1
        print(f"  {fname} (Font[{fidx}]): 95/95 ASCII PASS (slot0={slot_hits[0]}, slot1={slot_hits[1]}), metrics={mcm}")

    # Verify ALL 3,755/3,755 GB2312 Level-1 Han chars under zh-CN AND zh-TW!
    gb1 = []
    for l in range(0xB0, 0xD8):
        for tr in range(0xA1, 0xFF):
            try:
                gb1.append(bytes([l, tr]).decode('gb2312'))
            except UnicodeDecodeError:
                pass
    assert len(gb1) == 3755

    for fidx, fname in [(2, 'zh-CN'), (0, 'zh-TW')]:
        assert ec.set_primary(faces_c[fidx]) == 0
        hits = [0, 0, 0, 0]
        for ch in gb1:
            sl, f, gid = ec.lookup_4slot(ord(ch))
            assert sl is not None and gid != 0, f"{fname} missing GB2312 L1 char {ch} (U+{ord(ch):04X})"
            hits[sl] += 1
        print(f"  {fname}: 3755/3755 GB2312 L1 PASS via real ARM64 cmap (slots={hits})")

    # Verify real ARM64 glyph outline decoding across diverse samples (SC under Font[2], TC under Font[0])!
    assert ec.set_primary(faces_c[2]) == 0
    sample_sc = (
        "实况照片: 就绪 快门声音: 开启 实况回放按键: REC 竖屏录像 C64 滤镜槽 胶片模拟 / HDF "
        "状态 运行中 已用 异常 存档代 提交 上限 重启生效 富士 柯达 徕卡 禄来 哈苏 经典铬 颗粒 光晕 褪色 "
        "—·《》【】「」『』‘’“”：；！？，。、（）…～ Fujifilm Activation queued 'YY MM DD j=q@_^`{|}~"
    )
    decoded_per_slot = [0, 0, 0, 0]
    for ch in set(sample_sc):
        sl, f, gid = ec.lookup_4slot(ord(ch))
        assert sl is not None and gid != 0, f"SC sample char {ch!r} (U+{ord(ch):04X}) missing under Font[2]!"
        rc = ec.decode_glyph_in_face(f, gid)
        assert rc == 0, f"Nikon ARM64 decode_glyph failed (rc={rc}) for SC {ch!r} slot={sl} gid={gid}"
        decoded_per_slot[sl] += 1
    print(f"  SC sample ARM64 outline decode PASSED on {sum(decoded_per_slot)} unique chars (slots={decoded_per_slot})")

    assert ec.set_primary(faces_c[0]) == 0
    sample_tc = (
        "實況照片: 就緒 快門聲音: 開啟 實況回放按鍵: REC 直拍錄影 C64 濾鏡槽 膠片模擬 / HDF "
        "狀態 運行中 已用 異常 存檔代 提交 上限 重啟生效"
    )
    tc_decoded_per_slot = [0, 0, 0, 0]
    for ch in set(sample_tc):
        sl, f, gid = ec.lookup_4slot(ord(ch))
        assert sl is not None and gid != 0, f"TC sample char {ch!r} (U+{ord(ch):04X}) missing under Font[0]!"
        rc = ec.decode_glyph_in_face(f, gid)
        assert rc == 0, f"Nikon ARM64 decode_glyph failed (rc={rc}) for TC {ch!r} slot={sl} gid={gid}"
        tc_decoded_per_slot[sl] += 1
    print(f"  TC sample ARM64 outline decode PASSED on {sum(tc_decoded_per_slot)} unique chars (slots={tc_decoded_per_slot})")
    assert ec.set_primary(faces_c[2]) == 0

    exh1 = exh2 = 0
    if exhaustive:
        n1 = hif_glyph_count(cand_img, CJK1_VA)
        n2 = hif_glyph_count(cand_img, CJK2_VA)
        for gid in range(1, n1 + 1):
            rc = ec.decode_glyph_in_face(s_zh[2], gid)
            assert rc == 0, f"CJK_EXT_1 glyph {gid} decode failed (rc={rc})"
            exh1 += 1
        for gid in range(1, n2 + 1):
            rc = ec.decode_glyph_in_face(s_zh[3], gid)
            assert rc == 0, f"CJK_EXT_2 glyph {gid} decode failed (rc={rc})"
            exh2 += 1
        print(f"  Exhaustive ARM64 outline decode PASSED: CJK1={exh1}, CJK2={exh2}, total={exh1 + exh2}")

    # Verify non-CJK Font[16] clears Slots 1..3 = NULL!
    assert ec.set_primary(faces_c[16]) == 0
    assert ec.slots() == [faces_c[16], 0, 0, 0]
    assert eb.set_primary(faces_b[16]) == 0
    assert ec.metrics() == eb.metrics()

    # Verify cached re-select (0 allocs) & NULL face clear
    allocs_before = ec.allocs
    assert ec.set_primary(faces_c[2]) == 0
    assert ec.allocs == allocs_before, "Expected zero new allocations on cached CJK re-select!"
    assert ec.set_primary(0) == 0
    assert ec.slots() == [0, 0, 0, 0]
    print("  English Font[16] clear, cached re-select (0 allocs), & NULL face clear PASSED!")

    return (
        dict(gb2312_l1_zh_cn=3755, gb2312_l1_zh_tw=3755, ascii_all_cjk=95,
             metrics_zh_cn=list(m_c), decoded_sample_slots=decoded_per_slot),
        exh1, exh2
    )


def test_slots_lang_svc(cand_img):
    print("\n=== [2] Testing slots.state ops 13/14/15/16 ===")
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    size = (len(cand_img) + 0xFFF) & ~0xFFF
    u.mem_map(BASE, size, UC_PROT_ALL)
    u.mem_write(BASE, cand_img)
    u.mem_map(FLAG_PAGE, 0x2000, UC_PROT_ALL)
    u.mem_map(LANG_PAGE, 0x2000, UC_PROT_ALL)
    u.mem_map(STACK_TOP - 0x10000, 0x10000, UC_PROT_ALL)
    u.mem_map(SCRATCH, 0x1000, UC_PROT_ALL)

    def run_svc(op, val=0, lang=8, flag=0):
        w32(u, PORTRAIT_FLAG, flag)
        w32(u, LANG_VA, lang)
        sp = STACK_TOP - 0x200
        u.mem_write(sp + 0x10, struct.pack('<4I', 0, 0, 0, val))
        u.mem_write(SCRATCH, b'\x00' * 0x40)
        u.reg_write(UC_ARM64_REG_X8, op)
        u.reg_write(UC_ARM64_REG_X0, 1)  # FX_ARGUMENT
        u.reg_write(UC_ARM64_REG_SP, sp)
        u.reg_write(UC_ARM64_REG_X19, SCRATCH)
        u.emu_start(0x401FE200, 0x401A7898, count=100)
        assert u.reg_read(UC_ARM64_REG_PC) == 0x401A7898
        return (u.reg_read(UC_ARM64_REG_W0), r64(u, SCRATCH + 0x10), r64(u, SCRATCH + 0x18), r32(u, PORTRAIT_FLAG))

    for lang in (0, 1, 8, 9, 10, 35):
        rc, v0, v1, _ = run_svc(15, lang=lang)
        assert rc == 0 and v0 == lang and v1 == 0x4C414E47, f"op15 failed for lang={lang}: {(rc, v0, hex(v1))}"
    rc, _, _, fl = run_svc(13, val=1, flag=0)
    assert rc == 0 and fl == 1
    rc, v0, v1, _ = run_svc(14, flag=1)
    assert rc == 0 and v0 == 1 and v1 == 0x50525431
    rc, v0, v1, _ = run_svc(16, lang=8)
    assert rc == 1 and v0 == 0 and v1 == 0
    print("  slots.state op 15 (SLOTS_LANG_GET), ops 13/14 (Portrait), op 16 (FX_ARGUMENT) ALL PASSED!")


def test_menu_text(cand_img, orig_text_va):
    print(f"\n=== [3] Testing v68_menu_text (0x4032e130 -> dynamic ORIG_TEXT_VA={hex(orig_text_va)}) ===")
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    size = (len(cand_img) + 0xFFF) & ~0xFFF
    u.mem_map(BASE, size, UC_PROT_ALL)
    u.mem_write(BASE, cand_img)
    # Map initialized .data + BSS (0xA02800000..0xA04E00000) so 0x403dbdb4 & native text tables work.
    u.mem_map(0xA02800000, 0x2600000, UC_PROT_ALL)
    u.mem_write(0xA02808000, cand_img[0x4B13000:0x4DC0000])
    # Point Nikon runtime text directory globals (0xa02f2ac48 / 0xa02f2ac38) at cand_img tables & blobs!
    w64(u, 0xA02F2AC48, 0x4327C5A0)
    w64(u, 0xA02F2AC38, 0x439DAF58)
    u.mem_map(STACK_TOP - 0x10000, 0x10000, UC_PROT_ALL)
    u.mem_map(SCRATCH, 0x4000, UC_PROT_ALL)
    u.mem_map(0x7FFF0000, 0x10000, UC_PROT_ALL)
    u.mem_write(END, struct.pack('<I', 0xD503201F))

    # Fake Nikon menu stack for 0x403dbdb4():
    #   *(uint64_t*)0xa032b5160 -> stack_obj (stride=8, depth=1, entries=stack_entries)
    #   stack_entries[0] -> page_desc_ptr -> *(uint32_t*)page_desc = (page << 16) | group
    STACK_OBJ = SCRATCH + 0x2000
    STACK_ENT = SCRATCH + 0x2040
    PAGE_DESC = SCRATCH + 0x2080

    def set_menu_page(group, page):
        if group is None:
            w64(u, 0xA032B5160, 0)
            return
        w32(u, PAGE_DESC, ((page & 0xFFFF) << 16) | (group & 0xFFFF))
        w64(u, STACK_ENT, PAGE_DESC)
        w32(u, STACK_OBJ + 0, 8)
        w32(u, STACK_OBJ + 8, 1)
        w64(u, STACK_OBJ + 16, STACK_ENT)
        w64(u, 0xA032B5160, STACK_OBJ)

    set_menu_page(None, None)

    # Stub dynamically resolved orig_text_va! If v68_menu_text calls a stale V67 address instead,
    # it will NOT hit this stub and execution will fail immediately.
    stub_str_va = SCRATCH + 0x1000
    stub_str_len = [0]
    orig_hits = [0]
    u.mem_write(orig_text_va, struct.pack('<I', 0xD65F03C0))  # RET

    def on_orig(uc, addr, sz, _):
        orig_hits[0] += 1
        out_ptr = uc.reg_read(UC_ARM64_REG_X2)
        out_len = uc.reg_read(UC_ARM64_REG_X3)
        w64(uc, out_ptr, stub_str_va)
        w32(uc, out_len, stub_str_len[0])

    u.hook_add(UC_HOOK_CODE, on_orig, begin=orig_text_va, end=orig_text_va + 4)

    callee_regs = [
        UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X21, UC_ARM64_REG_X22,
        UC_ARM64_REG_X23, UC_ARM64_REG_X24, UC_ARM64_REG_X25, UC_ARM64_REG_X26,
        UC_ARM64_REG_X27, UC_ARM64_REG_X28, UC_ARM64_REG_X29,
    ]
    sentinels = [0x1111222233330000 | (i + 1) for i in range(len(callee_regs))]

    def query(lang, text_id, english_from_orig, expect_orig=True):
        enc = english_from_orig.encode('utf-16le')
        u.mem_write(stub_str_va, enc + b'\x00\x00')
        stub_str_len[0] = len(enc)
        out_p, out_n = SCRATCH + 0x100, SCRATCH + 0x108
        w64(u, out_p, 0)
        w32(u, out_n, 0)
        sp0 = STACK_TOP - 0x100
        u.reg_write(UC_ARM64_REG_SP, sp0)
        u.reg_write(UC_ARM64_REG_X30, END)
        u.reg_write(UC_ARM64_REG_X0, lang)
        u.reg_write(UC_ARM64_REG_X1, text_id)
        u.reg_write(UC_ARM64_REG_X2, out_p)
        u.reg_write(UC_ARM64_REG_X3, out_n)
        for reg, val in zip(callee_regs, sentinels):
            u.reg_write(reg, val)
        before_hits = orig_hits[0]
        u.emu_start(MENU_TEXT_HOOK_VA, END, count=10000)
        assert u.reg_read(UC_ARM64_REG_PC) == END
        if expect_orig:
            assert orig_hits[0] == before_hits + 1, f"Did not call dynamic ORIG_TEXT_VA {hex(orig_text_va)}!"
        else:
            assert orig_hits[0] == before_hits, f"Expected native NFP bypass, but called ORIG_TEXT_VA!"
        assert u.reg_read(UC_ARM64_REG_SP) == sp0, "SP not conserved!"
        for reg, val in zip(callee_regs, sentinels):
            assert u.reg_read(reg) == val, f"Callee-saved reg clobbered!"
        p = r64(u, out_p)
        n = r32(u, out_n)
        return bytes(u.mem_read(p, n)).decode('utf-16le')

    cases = [
        (3168, "On",                      "开启",               "開啟"),
        (3169, "Off",                     "关闭",               "關閉"),
        (3156, "Activate",                "激活",               "激活"),
        (3165, "Modules",                 "已加载插件",         "已載入插件"),
        (3300, "portrait",                "竖屏录像",           "直拍錄影"),
        (3300, "slotcard",                "C64 滤镜槽",         "C64 濾鏡槽"),
        (3300, "filmsim",                 "胶片模拟 / HDF",     "膠片模擬 / HDF"),
        (3300, "Film Simulation",         "胶片模拟 / HDF",     "膠片模擬 / HDF"),
        (3300, "livephoto",               "实况照片",           "實況照片"),
        (3300, "Live Photo",              "实况照片",           "實況照片"),
        (3300, "lut",                     "3D LUT 调色",        "3D LUT 調色"),
        (3300, "3D LUT",                  "3D LUT 调色",        "3D LUT 調色"),
        (3300, "Next page",               "下一页",             "下一頁"),
    ]
    for tid, en, sc, tc in cases:
        assert query(1, tid, en) == en, f"EN changed for {tid} {en}"
        assert query(0, tid, en) == en, f"JA changed for {tid} {en}"
        got_sc = query(8, tid, en)
        got_tc = query(9, tid, en)
        assert got_sc == sc, f"SC mismatch for {tid} {en}: got {got_sc!r}, want {sc!r}"
        assert got_tc == tc, f"TC mismatch for {tid} {en}: got {got_tc!r}, want {tc!r}"

    # Verify Setup Menu top-level WonderLantern (3155) -> Plugin Manager (EN/other) / 插件管理 (ZH-CN/ZH-TW)
    assert query(1, 3155, "WonderLantern") == "Plugin Manager"
    assert query(0, 3155, "WonderLantern") == "Plugin Manager"
    assert query(8, 3155, "WonderLantern") == "插件管理"
    assert query(9, 3155, "WonderLantern") == "插件管理"

    # Verify stock IDs (< 3154) and NFP IDs (3180..3253, incl. v0.5.0 3225..3249) NEVER get touched!
    assert query(8, 100, "On") == "On"
    assert query(8, 100, "filmsim") == "filmsim"
    assert query(8, 3217, "S-Log3 / S-Gamut3.Cine") == "S-Log3 / S-Gamut3.Cine"
    assert query(8, 3218, "Off") == "Off"
    assert query(8, 3218, "Film Simulation") == "Film Simulation"
    assert query(8, 3232, "N-RAW 12位 3:2(NEV)") == "N-RAW 12位 3:2(NEV)"
    assert query(8, 3236, "SDR文件格式") == "SDR文件格式"
    assert query(8, 3238, "4:3  (32x24)") == "4:3  (32x24)"

    # Verify context-aware NFP 3154..3179 bypass (Anamorphic 0xB6, Video Menu 0xFE, H.265 0x17D)
    # vs Setup Menu (0x0297) and Fenestra/Live/PB extension pages ([0x7000, 0x9500))!
    # 1. Setup Menu (0x0297): 3154/3155 stay Fenestra / 插件管理 / Plugin Manager
    set_menu_page(2, 0x0297)
    assert query(8, 3154, "Fenestra") == "Fenestra"
    assert query(8, 3155, "WonderLantern") == "插件管理"
    assert query(9, 3155, "WonderLantern") == "插件管理"
    assert query(1, 3155, "WonderLantern") == "Plugin Manager"

    # 2. Fenestra extension pages (0x7000, 0x7001, 0x704A): 3154..3179 stay Fenestra; removed static Live/PB keys pass through
    for ext_page in (0x7000, 0x7001, 0x704A):
        set_menu_page(2, ext_page)
        assert query(8, 3155, "WonderLantern") == "插件管理"
        assert query(8, 3165, "Modules") == "已加载插件"
        assert query(8, 3168, "On") == "开启"
        assert query(8, 3169, "Off") == "关闭"
        assert query(8, 3160, "Live playback key: REC") == "Live playback key: REC"
        assert query(8, 3163, "Shutter sound: On") == "Shutter sound: On"
        assert query(8, 3177, "Live Photo: Off") == "Live Photo: Off"

    # 3. Anamorphic submenu (0x00B6): 3154..3160 resolve from native NFP table (1.25x..2.00x), bypassing ORIG_TEXT_VA!
    set_menu_page(2, 0x00B6)
    expected_ratios = ['1.25x', '1.30x', '1.33x', '1.50x', '1.60x', '1.80x', '2.00x']
    for idx, ratio in enumerate(expected_ratios):
        for lang in (1, 8, 9):
            got = query(lang, 3154 + idx, "WonderLantern", expect_orig=False)
            assert got == ratio, f"Anamorphic 0xB6 lang={lang} id={3154+idx}: got {got!r}, want {ratio!r}"

    # 4. Video Recording Menu (0x00FE) & H.265 submenu (0x017D): 3161..3179 resolve from native NFP table!
    set_menu_page(2, 0x00FE)
    assert 'H.265' in query(1, 3161, "REC", expect_orig=False)
    assert 'H.265' in query(8, 3161, "REC", expect_orig=False)
    assert 'H.265' in query(9, 3161, "REC", expect_orig=False)

    set_menu_page(2, 0x017D)
    assert 'H.265' in query(8, 3161, "REC", expect_orig=False)
    assert query(8, 3162, "Live playback key", expect_orig=False) == "100%"
    assert query(8, 3163, "Shutter sound: On", expect_orig=False) == "60%"
    assert query(8, 3164, "No loaded modules", expect_orig=False) == "45%"
    assert query(8, 3165, "Modules", expect_orig=False) == "30%"
    assert query(8, 3166, "Auto activate", expect_orig=False) == "20%"
    assert query(8, 3177, "Live Photo: Off", expect_orig=False) == "100%  150 Mbps"
    assert query(8, 3179, "Active", expect_orig=False) == "45%  68 Mbps"
    set_menu_page(None, None)

    print(f"  All {len(cases)} x 4 languages + FilmSim + NFP 3154..3179/3225..3249 context routing + ABI PASSED!")
    return len(cases)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base-main', '--v66-main', dest='base_main', type=Path, required=True)
    ap.add_argument('--candidate-main', '--v67-main', '--v68-main', dest='candidate_main', type=Path, required=True)
    ap.add_argument('--expected-base-sha256', type=str, default=None)
    ap.add_argument('--expected-candidate-sha256', type=str, default=None)
    ap.add_argument('--skip-exhaustive', action='store_true')
    ap.add_argument('--out-json', type=Path)
    a = ap.parse_args()

    base = a.base_main.read_bytes()
    cand = a.candidate_main.read_bytes()
    base_sha = hashlib.sha256(base).hexdigest()
    cand_sha = hashlib.sha256(cand).hexdigest()
    if a.expected_base_sha256:
        assert base_sha == a.expected_base_sha256.lower(), f"Base SHA mismatch: {base_sha}"
    if a.expected_candidate_sha256:
        assert cand_sha == a.expected_candidate_sha256.lower(), f"Candidate SHA mismatch: {cand_sha}"

    menu_word = struct.unpack_from('<I', base, MENU_TEXT_HOOK_VA - BASE)[0]
    orig_text_va = decode_b(MENU_TEXT_HOOK_VA, menu_word)
    assert LIVE_PRIMARY_LO <= orig_text_va < LIVE_PRIMARY_HI, f"Invalid orig_text_va {hex(orig_text_va)}"

    font_res, exh1, exh2 = test_font_engine(cand, base, exhaustive=not a.skip_exhaustive)
    test_slots_lang_svc(cand)
    menu_cases = test_menu_text(cand, orig_text_va)

    res = {
        'passed': True,
        'base_main_sha256': base_sha,
        'candidate_main_sha256': cand_sha,
        'orig_text_va': hex(orig_text_va),
        'font_engine': font_res,
        'slots_lang_svc': True,
        'menu_text': True,
        'menu_text_cases': menu_cases,
        'filmsim_menu_keys_verified': ['filmsim', 'Film Simulation'],
        'exhaustive_decode_cjk1': exh1,
        'exhaustive_decode_cjk2': exh2,
        'exhaustive_decode_total': exh1 + exh2,
    }
    if a.out_json:
        a.out_json.parent.mkdir(parents=True, exist_ok=True)
        a.out_json.write_text(json.dumps(res, indent=2) + '\n', encoding='utf-8')
    print("\nALL UNICORN ARM64 TESTS PASSED!")


if __name__ == '__main__':
    main()
