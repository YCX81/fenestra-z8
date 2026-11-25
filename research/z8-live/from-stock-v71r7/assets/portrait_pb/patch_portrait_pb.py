#!/usr/bin/env python3
"""Apply the gated portrait video playback patch on top of V63-Live1280 (or compatible) MAIN.

All externally visible behaviors are gated on *(uint32_t *)0xA116D3480 (portrait.wlm):
  - gate == 0: 100% stock Nikon / baseline behavior across recording & all playback APIs
  - gate != 0:
      1. tkhd_latch    (0x401FE280): latch start-of-recording pose into 0xA116D3484
      2. nctg_0018     (0x406D2A60): consume latch / pose -> write NCTG Tag 0x0018 (1 / 8 / 6)
      3. dcf_mov       (0x406BFF7C): preserve ColorSpace=0, always populate DCF orient (8->1, 6->2, else 0)
                                     so cold-boot card scan before portrait.wlm load & runtime gate toggles work
      4. item_orient   (0x407B5AE8): unified exit of 0x407B5A00 (post-cache): force 0 for movies when gate==0
      5. backend_rot   (0x4079F738): keep w19 for movies in 0x4079F718 (23 callers) when gate!=0
      6. backend_rot_2 (0x4079F798): keep movie rotation in twin resolver 0x4079F790 (single-frame rect
                                     0x4079F124 & HW rotate job 0x407B35FC callers) when gate!=0
      7. thumb_orient  (0x407CADF4): post-lock gate on 0x407CADD4 (0x040E0013 thumb/JPEG orient) when gate==0
      8. ui_orient     (0x4068D658): read [0xA032CDA00 + 92] for movies in (3, 0x552) when gate!=0
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BASE = 0x40160000
BUILD_ID_ADDR = 0x401FE060
EXPECTED_SIZE = 81807709
EXPECTED_INPUT_SHA256 = '1126f043ed7df1c0acd69368cb5328ac9c1896f04e4caef5b873fe568bb94866'

GATE_ADDR = 0xA116D3480
LATCH_ADDR = 0xA116D3484
POSE_ADDR = 0xA03228244

PRIMARY_START = 0x401E2000
CAVE_START = 0x401FCD00
CAVE_LIMIT = 0x401FCF40  # Menu context helper starts at 0x401FCF40

COND_EQ = 0
COND_NE = 1

HOOKS = {
    'tkhd_latch': 0x401FE280,
    'nctg_0018': 0x406D2A60,
    'dcf_mov': 0x406BFF7C,
    'item_orient': 0x407B5AE8,
    'backend_rot': 0x4079F738,
    'backend_rot_2': 0x4079F798,
    'thumb_orient': 0x407CADF4,
    'ui_orient': 0x4068D658,
}

GUARDS = {
    0x401FE264: bytes.fromhex(
        '090140b90a9086d2aa2da2f24a01c0f24a0140b95f010071e903891a5cd62b14'
    ),
    0x406D2A60: bytes.fromhex('e8030032e0030032289400b9c0035fd6'),
    0x406BFF74: bytes.fromhex('e00313aae1031f2a35f1ff97e1434039'),
    0x407B5AE0: bytes.fromhex('2a0100b9000100b9f37b42a9f55341a9ffc30091c0035fd6'),
    0x4079F730: bytes.fromhex('a76b00941f000072f313931a2cbaff97'),
    0x4079F790: bytes.fromhex('f37bbfa9f303002a8101003714baff97'),
    0x407CADE8: bytes.fromhex('e83f02f9400000b4233f0294e80a001209090051'),
    0x4068D64C: bytes.fromhex(
        'e0031f2ab30000341f050071600000540c9cfe97005c40b9f37bc1a8c0035fd6'
    ),
}

TARGETS = {
    'tkhd_resume': 0x40CF3BF0,
    'dcf_set_colorspace': 0x406BC450,
    'dcf_set_orient': 0x406BC47C,
    'is_movie_file': 0x407BA5CC,
    'backend_resume': 0x4079F73C,
    'backend2_resume': 0x4079F79C,
    'backend2_zero': 0x4079F7C8,
    'thumb_resume': 0x407CADF8,
    'ui_read_orient': 0x4068D65C,
    'ui_ret': 0x4068D664,
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def need(ok, message):
    if not ok:
        raise ValueError(message)


def _check_branch_off(pc, target, limit):
    diff = target - pc
    need(diff % 4 == 0 and -limit <= diff < limit, f'branch out of range: {pc:#x} -> {target:#x}')
    return diff >> 2


def enc_b(pc, target, link=False):
    imm26 = _check_branch_off(pc, target, 0x08000000) & 0x03FFFFFF
    return (0x94000000 if link else 0x14000000) | imm26


def enc_b_cond(pc, target, cond):
    imm19 = _check_branch_off(pc, target, 0x00100000) & 0x7FFFF
    return 0x54000000 | (imm19 << 5) | (cond & 0xF)


def enc_cbz(pc, target, rt, nz=False, sf=0):
    imm19 = _check_branch_off(pc, target, 0x00100000) & 0x7FFFF
    return 0x34000000 | (sf << 31) | ((1 if nz else 0) << 24) | (imm19 << 5) | rt


def enc_tbz(pc, target, rt, bit, nz=False):
    diff = target - pc
    need(diff % 4 == 0 and -0x8000 <= diff < 0x8000, f'tbz/tbnz out of range: {pc:#x} -> {target:#x}')
    imm14 = (diff >> 2) & 0x3FFF
    b5 = (bit >> 5) & 1
    b40 = bit & 0x1F
    base = 0x37000000 if nz else 0x36000000
    return base | (b5 << 31) | (b40 << 19) | (imm14 << 5) | rt


def enc_tbnz(pc, target, rt, bit):
    return enc_tbz(pc, target, rt, bit, nz=True)


def enc_movz(rd, imm16, hw=0, sf=1):
    return 0x52800000 | (sf << 31) | (hw << 21) | ((imm16 & 0xFFFF) << 5) | rd


def enc_movk(rd, imm16, hw=0, sf=1):
    return 0x72800000 | (sf << 31) | (hw << 21) | ((imm16 & 0xFFFF) << 5) | rd


def enc_mov64(rd, value):
    need(0 <= value <= 0xFFFFFFFFFFFF, f'constant exceeds 48-bit encoding: {value:#x}')
    return [
        enc_movz(rd, value & 0xFFFF, hw=0, sf=1),
        enc_movk(rd, (value >> 16) & 0xFFFF, hw=1, sf=1),
        enc_movk(rd, (value >> 32) & 0xFFFF, hw=2, sf=1),
    ]


def enc_mov_reg(rd, rm, sf=1):
    return 0x2A0003E0 | (sf << 31) | (rm << 16) | rd


def enc_ldr_w(rt, rn, offset=0):
    need(offset % 4 == 0 and 0 <= offset < 16384, f'invalid ldr offset: {offset}')
    return 0xB9400000 | ((offset // 4) << 10) | (rn << 5) | rt


def enc_str_w(rt, rn, offset=0):
    need(offset % 4 == 0 and 0 <= offset < 16384, f'invalid str offset: {offset}')
    return 0xB9000000 | ((offset // 4) << 10) | (rn << 5) | rt


def enc_stp_pre(rt1, rt2, rn, simm):
    need(simm % 8 == 0 and -512 <= simm <= 504, f'invalid stp simm: {simm}')
    imm7 = (simm // 8) & 0x7F
    return 0xA9800000 | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt1


def enc_ldp_post(rt1, rt2, rn, simm):
    need(simm % 8 == 0 and -512 <= simm <= 504, f'invalid ldp simm: {simm}')
    imm7 = (simm // 8) & 0x7F
    return 0xA8C00000 | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt1


def enc_ldp_off(rt1, rt2, rn, simm):
    need(simm % 8 == 0 and -512 <= simm <= 504, f'invalid ldp off simm: {simm}')
    imm7 = (simm // 8) & 0x7F
    return 0xA9400000 | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt1


def enc_add_sp(imm12):
    need(0 <= imm12 < 4096, f'invalid add sp imm: {imm12}')
    return 0x910003FF | (imm12 << 10)


def enc_and_w_ff(rd, rn):
    return 0x12001C00 | (rn << 5) | rd


def enc_and_w_7(rd, rn):
    return 0x12000800 | (rn << 5) | rd


def enc_orr_w_bit8(rd, rn):
    return 0x32180000 | (rn << 5) | rd


def enc_tst_w_1(rn):
    return 0x7200001F | (rn << 5)


def enc_cmp_w_imm(rn, imm12):
    need(0 <= imm12 < 4096, f'invalid cmp imm: {imm12}')
    return 0x7100001F | (imm12 << 10) | (rn << 5)


def enc_cset_eq_w(rd):
    return 0x1A9F17E0 | rd


def enc_csel_w(rd, rn, rm, cond):
    return 0x1A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd


def enc_ret():
    return 0xD65F03C0


class AsmBlock:
    """Two-pass AArch64 micro-assembler with local labels."""

    def __init__(self, start_va):
        self.start_va = start_va
        self.items = []
        self.labels = {}

    @property
    def cur_va(self):
        return self.start_va + len(self.items) * 4

    def label(self, name):
        need(name not in self.labels, f'duplicate label {name}')
        self.labels[name] = self.cur_va

    def emit(self, word_or_fn):
        if isinstance(word_or_fn, list):
            for w in word_or_fn:
                self.items.append((self.cur_va, w))
        else:
            self.items.append((self.cur_va, word_or_fn))

    def finish(self):
        words = []
        for va, item in self.items:
            word = item(va, self.labels) if callable(item) else item
            words.append(struct.pack('<I', word))
        return b''.join(words), self.cur_va


def build_cave(start_va=CAVE_START):
    """Assemble the 8 gated helper routines in the 0x401FCD00..0x401FCF40 cave."""
    a = AsmBlock(start_va)
    entries = {}

    # 1. cave_tkhd_latch: w9=gated pose, w10=gate, x11 dead scratch
    entries['tkhd_latch'] = a.cur_va
    a.emit(enc_mov64(11, LATCH_ADDR))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['tkhd_clear'], 10, nz=False, sf=0))
    a.emit(enc_and_w_ff(10, 9))
    a.emit(enc_orr_w_bit8(10, 10))
    a.emit(lambda va, lbl: enc_b(va, lbl['tkhd_store']))
    a.label('tkhd_clear')
    a.emit(enc_mov_reg(10, 31, sf=0))
    a.label('tkhd_store')
    a.emit(enc_str_w(10, 11, 0))
    a.emit(lambda va, _: enc_b(va, TARGETS['tkhd_resume']))

    # 2. cave_nctg_0018: write [x1, #148] = 1 / 8 / 6, return w0 = 1
    entries['nctg_0018'] = a.cur_va
    a.emit(enc_movz(8, 1, sf=0))
    a.emit(enc_mov64(9, GATE_ADDR))
    a.emit(enc_ldr_w(10, 9, 0))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['nctg_done'], 10, nz=False, sf=0))
    a.emit(enc_ldr_w(10, 9, 4))
    a.emit(enc_str_w(31, 9, 4))
    a.emit(lambda va, lbl: enc_tbnz(va, lbl['nctg_has_latch'], 10, 8))
    a.emit(enc_mov64(11, POSE_ADDR))
    a.emit(enc_ldr_w(10, 11, 0))
    a.emit(lambda va, lbl: enc_b(va, lbl['nctg_map']))
    a.label('nctg_has_latch')
    a.emit(enc_and_w_ff(10, 10))
    a.label('nctg_map')
    a.emit(enc_cmp_w_imm(10, 1))
    a.emit(lambda va, lbl: enc_b_cond(va, lbl['nctg_check_2'], COND_NE))
    a.emit(enc_movz(8, 8, sf=0))
    a.emit(lambda va, lbl: enc_b(va, lbl['nctg_done']))
    a.label('nctg_check_2')
    a.emit(enc_cmp_w_imm(10, 2))
    a.emit(lambda va, lbl: enc_b_cond(va, lbl['nctg_done'], COND_NE))
    a.emit(enc_movz(8, 6, sf=0))
    a.label('nctg_done')
    a.emit(enc_movz(0, 1, sf=0))
    a.emit(enc_str_w(8, 1, 148))
    a.emit(enc_ret())

    # 3. cave_dcf_mov: preserve dcf_set_colorspace(x19, 0) and always populate DCF orient
    #    from NCTG 0x0018 (8 -> 1, 6 -> 2, 1/other -> 0). All playback consumers gate on GATE_ADDR,
    #    so cold-boot DCF card scans before portrait.wlm loads and runtime Off<->On toggles work.
    entries['dcf_mov'] = a.cur_va
    a.emit(enc_stp_pre(20, 30, 31, -16))
    a.emit(enc_ldr_w(20, 31, 64))
    a.emit(lambda va, _: enc_b(va, TARGETS['dcf_set_colorspace'], link=True))
    a.emit(enc_cmp_w_imm(20, 8))
    a.emit(enc_cset_eq_w(9))
    a.emit(enc_cmp_w_imm(20, 6))
    a.emit(enc_movz(8, 2, sf=0))
    a.emit(enc_csel_w(1, 8, 9, COND_EQ))
    a.emit(enc_mov_reg(0, 19, sf=1))
    a.emit(lambda va, _: enc_b(va, TARGETS['dcf_set_orient'], link=True))
    a.emit(enc_ldp_post(20, 30, 31, 16))
    a.emit(enc_ret())

    # 4. cave_item_orient: unified epilogue of 0x407B5A00 (post-cache).
    #    If w0 != 0 and gate == 0 and is_movie_file([x19]), return 0 without poisoning cache.
    entries['item_orient'] = a.cur_va
    a.emit(lambda va, lbl: enc_cbz(va, lbl['io_epilogue'], 0, nz=False, sf=0))
    a.emit(enc_mov64(9, GATE_ADDR))
    a.emit(enc_ldr_w(9, 9, 0))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['io_epilogue'], 9, nz=True, sf=0))
    a.emit(enc_mov_reg(20, 0, sf=0))
    a.emit(enc_ldr_w(0, 19, 0))
    a.emit(lambda va, _: enc_b(va, TARGETS['is_movie_file'], link=True))
    a.emit(enc_tst_w_1(0))
    a.emit(enc_csel_w(0, 31, 20, COND_NE))
    a.label('io_epilogue')
    a.emit(enc_ldp_off(19, 30, 31, 32))
    a.emit(enc_ldp_off(21, 20, 31, 16))
    a.emit(enc_add_sp(48))
    a.emit(enc_ret())

    # 5. cave_backend_rot: keep w19 for photos or gate != 0 movies; zero w19 for gate == 0 movies
    entries['backend_rot'] = a.cur_va
    a.emit(lambda va, lbl: enc_b_cond(va, lbl['be_resume'], COND_EQ))
    a.emit(enc_mov64(9, GATE_ADDR))
    a.emit(enc_ldr_w(9, 9, 0))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['be_resume'], 9, nz=True, sf=0))
    a.emit(enc_mov_reg(19, 31, sf=0))
    a.label('be_resume')
    a.emit(lambda va, _: enc_b(va, TARGETS['backend_resume']))

    # 6. cave_backend_rot_2: twin resolver 0x4079F790 (w19=item_orient, w1=is_movie).
    #    Photos (w1&1==0) or gate!=0 movies resume at 0x4079F79C; gate==0 movies return 0 at 0x4079F7C8.
    entries['backend_rot_2'] = a.cur_va
    a.emit(lambda va, lbl: enc_tbz(va, lbl['be2_resume'], 1, 0, nz=False))
    a.emit(enc_mov64(9, GATE_ADDR))
    a.emit(enc_ldr_w(9, 9, 0))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['be2_resume'], 9, nz=True, sf=0))
    a.emit(lambda va, _: enc_b(va, TARGETS['backend2_zero']))
    a.label('be2_resume')
    a.emit(lambda va, _: enc_b(va, TARGETS['backend2_resume']))

    # 7. cave_thumb_orient: post-lock gate at 0x407CADF4 (after 0x407CADF0 releases DCF handle).
    #    If [sp, #1136] != 0 and gate == 0 and is_movie_file([x21]), zero [sp, #1136].
    entries['thumb_orient'] = a.cur_va
    a.emit(enc_ldr_w(8, 31, 1136))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['th_resume'], 8, nz=False, sf=0))
    a.emit(enc_mov64(9, GATE_ADDR))
    a.emit(enc_ldr_w(9, 9, 0))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['th_resume'], 9, nz=True, sf=0))
    a.emit(enc_ldr_w(0, 21, 0))
    a.emit(lambda va, _: enc_b(va, TARGETS['is_movie_file'], link=True))
    a.emit(lambda va, lbl: enc_tbz(va, lbl['th_resume'], 0, 0, nz=False))
    a.emit(enc_str_w(31, 31, 1136))
    a.label('th_resume')
    a.emit(enc_and_w_7(8, 23))
    a.emit(lambda va, _: enc_b(va, TARGETS['thumb_resume']))

    # 8. cave_ui_orient: at entry NZCV from `cmp w8, #1`, w0 == 0, w19 != 0
    entries['ui_orient'] = a.cur_va
    a.emit(lambda va, lbl: enc_b_cond(va, lbl['ui_read'], COND_NE))
    a.emit(enc_mov64(9, GATE_ADDR))
    a.emit(enc_ldr_w(9, 9, 0))
    a.emit(lambda va, lbl: enc_cbz(va, lbl['ui_read'], 9, nz=True, sf=0))
    a.emit(lambda va, _: enc_b(va, TARGETS['ui_ret']))
    a.label('ui_read')
    a.emit(lambda va, _: enc_b(va, TARGETS['ui_read_orient']))

    cave_bytes, end_va = a.finish()
    return cave_bytes, end_va, entries


def patch(image, expected_sha256, build, recalculate_build_id=True, version='v63-portrait-pb'):
    """Validate guards & bounds on baseline MAIN, apply portrait playback hooks, return (image, facts)."""
    need(len(image) == EXPECTED_SIZE, f'unexpected MAIN size: {len(image)}')
    need(sha(image) == expected_sha256, 'input SHA256 mismatch')
    need(image[0x14EAA90:0x14EAA98] == image[-9:-1] == b'04.06.p0', 'identity mismatch')

    payload = build.get('live_payload_bytes')
    need(type(payload) is int and payload > 0, 'missing live_payload_bytes in build manifest')
    live_end = PRIMARY_START + payload
    need(live_end <= CAVE_START, f'live payload {live_end:#x} overlaps cave {CAVE_START:#x}')

    for address, expected in GUARDS.items():
        actual = image[address - BASE:address - BASE + len(expected)]
        need(actual == expected, f'guard differs at {address:#x}')

    need(
        image[CAVE_START - BASE:CAVE_LIMIT - BASE] == bytes(CAVE_LIMIT - CAVE_START),
        f'cave {CAVE_START:#x}..{CAVE_LIMIT:#x} is not zero',
    )

    cave_bytes, end_va, entries = build_cave(CAVE_START)
    need(end_va <= CAVE_LIMIT, f'cave overflow: {end_va:#x} > {CAVE_LIMIT:#x}')

    out = bytearray(image)
    out[CAVE_START - BASE:CAVE_START - BASE + len(cave_bytes)] = cave_bytes

    hook_words = {
        'tkhd_latch': enc_b(HOOKS['tkhd_latch'], entries['tkhd_latch'], link=False),
        'nctg_0018': enc_b(HOOKS['nctg_0018'], entries['nctg_0018'], link=False),
        'dcf_mov': enc_b(HOOKS['dcf_mov'], entries['dcf_mov'], link=True),
        'item_orient': enc_b(HOOKS['item_orient'], entries['item_orient'], link=False),
        'backend_rot': enc_b(HOOKS['backend_rot'], entries['backend_rot'], link=False),
        'backend_rot_2': enc_b(HOOKS['backend_rot_2'], entries['backend_rot_2'], link=False),
        'thumb_orient': enc_b(HOOKS['thumb_orient'], entries['thumb_orient'], link=False),
        'ui_orient': enc_b(HOOKS['ui_orient'], entries['ui_orient'], link=False),
    }
    for name, word in hook_words.items():
        addr = HOOKS[name]
        out[addr - BASE:addr - BASE + 4] = struct.pack('<I', word)

    pre_build_id_sha = sha(bytes(out))
    old_bid = struct.unpack_from('<I', image, BUILD_ID_ADDR - BASE)[0]
    new_bid = old_bid
    if recalculate_build_id:
        struct.pack_into('<I', out, BUILD_ID_ADDR - BASE, 0)
        new_bid = struct.unpack('<I', hashlib.sha256(out).digest()[:4])[0] | 0x01000000
        struct.pack_into('<I', out, BUILD_ID_ADDR - BASE, new_bid)

    allowed = [(CAVE_START - BASE, len(cave_bytes))]
    for addr in HOOKS.values():
        allowed.append((addr - BASE, 4))
    if recalculate_build_id:
        allowed.append((BUILD_ID_ADDR - BASE, 4))

    diff = [i for i, (a, b) in enumerate(zip(image, out)) if a != b]
    need(
        all(any(start <= i < start + count for start, count in allowed) for i in diff),
        'diff outside approved cave/hooks/build_id windows',
    )

    facts = {
        'version': version,
        'baseline_main_sha256': sha(image),
        'pre_build_id_sha256': pre_build_id_sha,
        'final_main_sha256': sha(bytes(out)),
        'old_build_id': hex(old_bid),
        'build_id': hex(new_bid),
        'gate_addr': hex(GATE_ADDR),
        'latch_addr': hex(LATCH_ADDR),
        'live_payload_end': hex(live_end),
        'cave': {
            'start': hex(CAVE_START),
            'end_exclusive': hex(end_va),
            'limit': hex(CAVE_LIMIT),
            'bytes': len(cave_bytes),
            'free_before_menu_helper': CAVE_LIMIT - end_va,
        },
        'entries': {k: hex(v) for k, v in entries.items()},
        'hooks': {
            k: {
                'address': hex(HOOKS[k]),
                'entry': hex(entries[k]),
                'before': image[HOOKS[k] - BASE:HOOKS[k] - BASE + 4].hex(),
                'after': out[HOOKS[k] - BASE:HOOKS[k] - BASE + 4].hex(),
            }
            for k in HOOKS
        },
        'changed_byte_count': len(diff),
    }
    return bytes(out), facts


def main():
    root = Path(__file__).resolve().parent
    v63 = root.parent / 'v63-live1280'
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, default=v63 / 'final/main.main')
    ap.add_argument('--expected-sha256', default=EXPECTED_INPUT_SHA256)
    ap.add_argument('--build-manifest', type=Path, default=v63 / 'build/build.json')
    ap.add_argument('--output', type=Path, default=root / 'final/main.main')
    ap.add_argument('--meta', type=Path, default=root / 'final/main.json')
    ap.add_argument('--version', default='v63-portrait-pb')
    a = ap.parse_args()

    image = a.input.read_bytes()
    build = json.loads(a.build_manifest.read_text())
    out, facts = patch(image, a.expected_sha256, build, recalculate_build_id=True, version=a.version)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(out)
    a.meta.write_text(json.dumps(facts, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(facts, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
