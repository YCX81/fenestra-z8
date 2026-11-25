#!/usr/bin/env python3
"""Stage 0 standalone patch module for V71.5:
Upgrade embedded NikonFirmwarePatch (NFP) from v0.4.0 (67807d5) or relocated v0.5.0
(edf1433) — or apply cleanly onto stock C3.11 — to full 4-group NFP v0.5.1 (25fefa1:
video-recording + log-colour + still-photo-tools + raw-precapture, including v0.5.1
`framing-fixes` for 4.1K 3:2 live-view/recording AF geometry, 3:2 wide-area AF point
cells/rows, and EVF 3:2 playback), cleanly relocated strictly below 0x40B50000 with
zero byte collisions against V71.5 Stage 0/1/2/3 banks (C64, patch_c64_imenu_ui,
FilmSim, Live, PortraitPB, FontCJK-ZhUI).

Relocations preserved (all 100% zero caves verified in V71.5):
  - Region A (sdr-heif + photo_shared): 0x40B70000..0x40B72000 -> 0x40AF8000..0x40AFA000 (-0x78000)
  - Region B (photo-areas Part 1):      0x40B7C000..0x40B85000 -> 0x40B12000..0x40B1B000 (-0x6A000)
  - Region C (photo-areas Part 2):      0x40B90000..0x40B97000 -> 0x40AF0000..0x40AF7000 (-0xA0000)
  - Region D (Q3_STASH RW-alias fix):   0x40950000 (0xA0FF50000) -> 0x40952000 (0xA0FF52000) (+0x2000)
                                        (fixes latent runtime write overlap onto Stage 3 cjk_ext_1.hif)

Commutative / superset special sites (never overwrite/downgrade Stage 0/3 patches):
  - 0x407138D0 (PLAYBACK_FIT):          preserves Stage 0 aspect_fix (b 0x4019ffc0 -> 0x409708f0)
                                        if present; otherwise writes v0.5.1 b 0x40968400 (evf_fit_hook).
  - 0x40CF4620..0x40CF4634 (Pool #6):   preserves Stage 0 5b superset 0x001C2000 (1280x960x1.5)
                                        if already >= 0x0018D800 (NEVER downgrades 0x1C2000 -> 0x18D800);
                                        raises stock 0x00151800 -> v0.5.1 0x0018D800 on standalone stock.
  - 0x41184D8C (loader_copy_end):       floor 0x40B50000 (preserves Stage 0 C64 0x40B8A000 / Stage 3 0x40C00000).

Public API for Stage 0 integration:
    apply(image: bytearray, *, bundle_dir: Path | None = None) -> dict
    verify_contract(image: bytes | bytearray) -> dict
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

BASE = 0x40160000
IMAGE_SIZE = 81807709
BUILD_ID_ADDR = 0x401FE060

STRING_TEXT_LO = 0x40A50000
STRING_TABLES = 0x40A7C000
STRING_ENTRIES = 3251  # native 0..3135 + spacer 3136 + NFP 3137..3249 + end sentinel

# Relocated addresses for contract verification
RELOC_CLOW_DESC = 0x40AF1040      # was 0x40B91040 (-0xA0000)
RELOC_HEIF_DISPATCH = 0x40AF9A00  # was 0x40B71A00 (-0x78000)
RELOC_HEIF_LO = 0x40AF8000
RELOC_HEIF_HI = 0x40AFA000
RELOC_AREA_MENU = 0x40B16000      # was 0x40B80000 (-0x6A000)
RELOC_AREA_ICONS = 0x40B1A000     # was 0x40B84000 (-0x6A000)
RELOC_Q3_STASH_VA = 0x40952000    # was 0x40950000 (+0x2000)
RELOC_Q3_STASH_RAM = 0xA0FF52000  # was 0xA0FF50000
Q3_STASH_LITERAL_VA = 0x409628F0

# v0.5.1 framing-fixes unit constants (open-gate tail cave 0x40968000..0x4096F000)
FRAMING_FIXES_VA = 0x40968000
FRAMING_FIXES_BYTES = 2096
FRAMING_FIXES_CAVE_END = 0x4096F000
FRAMING_FIXES_SHA256 = '4f7ad4576b5eaa6eb74c60943fabd218cb7cbe32db64f349e6a63ccc0381bbd2'
AF_GEOMETRY_ENTRY = 0x40968000
AF_FORWARD_ENTRY = 0x40968148
AF_INVERSE_ENTRY = 0x409681D0
EVF_FIT_HOOK_ENTRY = 0x40968400
POINTS_SOURCE_ENTRY = 0x40968600
ASPECT_FIX_ENTRY = 0x4019FFC0

AF_GEOMETRY_CALLS = (0x41055AC0, 0x410567DC)
AF_FORWARD_SITE = 0x41055B38
AF_INVERSE_SITE = 0x4105693C
AP_POINTS_SCALE_SITE = 0x409704B0
PLAYBACK_FIT_SITE = 0x407138D0

POOL6_SLOT_LO_VA = 0x40CF4620
POOL6_SLOT_HI_VA = 0x40CF4630
POOL6_MIDDLE_STOCK_HEX = 'eb031faa0f228252cc010c8b'
POOL6_STOCK_SLOT = 0x00151800      # 1280 * 720 * 1.5
POOL6_NFP_V051_SLOT = 0x0018D800   # 1280 * 848 * 1.5
POOL6_V71_SUPERSET_SLOT = 0x001C2000  # 1280 * 960 * 1.5

# Regions strictly off-limits for NFP v0.5.1 edits
FORBIDDEN_BANKS = (
    ('v71_low_caves', 0x40196000, 0x40200000),
    ('cjk_ext_1',     0x408C6000, 0x40951000),
    ('q3_stash_zero', 0x40952000, 0x40953000),
    ('filmsim',       0x409B0000, 0x409D8000),
    ('c64_and_cjk2',  0x40B50000, 0x40C00000),
)


def _sha(data: bytes | memoryview) -> str:
    return hashlib.sha256(data).hexdigest()


def _decode_loader_end(buf: bytes | bytearray) -> int:
    w0, w1 = struct.unpack_from('<II', buf, 0x41184D8C - BASE)
    if (w0 & 0xFFE0001F) != 0xD2800008 or (w1 & 0xFFE0001F) != 0xF2A00008:
        raise ValueError(f'Unexpected loader_copy_end instructions at 0x41184d8c: {w0:#010x} {w1:#010x}')
    return ((w0 >> 5) & 0xFFFF) | (((w1 >> 5) & 0xFFFF) << 16)


def _encode_loader_end(end: int) -> bytes:
    assert 0x408BF8F0 <= end <= 0x40C00000 and end % 4096 == 0, hex(end)
    w0 = 0xD2800008 | ((end & 0xFFFF) << 5)
    w1 = 0xF2A00008 | (((end >> 16) & 0xFFFF) << 5)
    return struct.pack('<II', w0, w1)


def _decode_pool6_slot(buf: bytes | bytearray) -> int:
    lo = struct.unpack_from('<I', buf, POOL6_SLOT_LO_VA - BASE)[0]
    hi = struct.unpack_from('<I', buf, POOL6_SLOT_HI_VA - BASE)[0]
    mid = bytes(buf[POOL6_SLOT_LO_VA - BASE + 4:POOL6_SLOT_HI_VA - BASE]).hex()
    if (lo & 0xFFE0001F) != 0x5280000D or (hi & 0xFFE0001F) != 0x72A0000D:
        raise ValueError(f'Unexpected MOVS Pool #6 slot instructions at 0x40cf4620/30: {lo:#010x} / {hi:#010x}')
    if mid != POOL6_MIDDLE_STOCK_HEX:
        raise ValueError(f'Unexpected intervening instructions at 0x40cf4624..0x40cf4630: {mid}')
    return ((lo >> 5) & 0xFFFF) | (((hi >> 5) & 0xFFFF) << 16)


def _decode_branch(at: int, word: int, *, expect_link: bool | None = None) -> int:
    op = word >> 26
    if op not in (0x05, 0x25):
        raise ValueError(f'Not B/BL at {at:#x}: {word:#010x}')
    if expect_link is True and op != 0x25:
        raise ValueError(f'Expected BL at {at:#x}, got B: {word:#010x}')
    if expect_link is False and op != 0x05:
        raise ValueError(f'Expected B at {at:#x}, got BL: {word:#010x}')
    imm = word & 0x3FFFFFF
    if imm & 0x2000000:
        imm -= 0x4000000
    return at + imm * 4


def verify_contract(image: bytes | bytearray) -> dict:
    """Independent structural verification for relocated full 4-group NFP v0.5.1."""
    def need(ok: bool, msg: str) -> None:
        if not ok:
            raise ValueError('NFP v0.5.1 contract failure: ' + msg)

    need(len(image) == IMAGE_SIZE, f'image size {len(image)} != {IMAGE_SIZE}')
    rd = lambda fmt, off: struct.unpack_from(fmt, image, off)

    # 1. Shared video menu (45 rows: 44 stock + 0x315A)
    rows, events, count = rd('<QQH', 0x4BCC2C0)
    need((rows, events, count) == (0x40A30000, 0x40A31000, 45), 'video menu allocation/count')
    ids = [rd('<I', rows - BASE + 44 * i + 24)[0] for i in range(count)]
    need(len(ids) == len(set(ids)) == 45, 'video menu uniqueness')
    need(ids.index(0x2E38) == ids.index(0x2FC1) + 1, 'Log row after file type')
    need(ids.index(0x311B) == ids.index(0x3118) + 1 and ids.index(0x315A) == ids.index(0x311B) + 1, 'quality rows order')
    for site in (0x40572940, 0x4057295C, 0x40572978):
        need(bytes(image[site - BASE:site - BASE + 4]) == bytes.fromhex('02008052'), f'menu visibility at {site:#x}')
    need(rd('<I', 0x40980F00 - BASE)[0] == 7, 'shared feature mask == 7')
    ptrs = rd('<4Q', 0x40980F08 - BASE)
    need(all(ptrs), 'all 4 shared video callbacks present')
    need(0x40980000 <= rd('<Q', 0x4BF3A80)[0] < 0x40980F00, 'shared refresh entry')

    # 2. All 36 language offset tables & all added strings 3137..3249
    for lang in range(36):
        table = rd('<Q', 0x311C5A0 + 8 * lang)[0]
        need(table == STRING_TABLES + lang * STRING_ENTRIES * 4, f'lang {lang} table VA {table:#x}')
        offsets = rd(f'<{STRING_ENTRIES}i', table - BASE)
        blob = rd('<Q', 0x387AF58 + 8 * lang)[0]
        for ident in range(3137, 3250):
            a, b = blob + offsets[ident], blob + offsets[ident + 1]
            need(STRING_TEXT_LO <= a < b <= STRING_TABLES, f'lang {lang} id {ident} bounds {a:#x}..{b:#x}')
            bytes(image[a - BASE:b - BASE]).decode('utf-16le')

    # 3. Loader boundary covers all relocated caves (< 0x40B50000)
    loader_end = _decode_loader_end(image)
    need(0x40B50000 <= loader_end <= 0x40C00000 and loader_end % 4096 == 0, f'loader_end {loader_end:#x}')

    # 4. Pre-capture, SDR HEIF, photo-areas, open-gate 11-row file-type hooks & relocated tables
    need(bytes(image[0x40EA3638 - BASE:0x40EA363C - BASE]) == bytes.fromhex('fd3bed97'), 'pre-capture writer hook')
    need(_decode_branch(0x403A036C, rd('<I', 0x403A036C - BASE)[0]) == RELOC_HEIF_DISPATCH, 'joint condition dispatch')
    for va in (0x40EC8D24, 0x40EC8D28, 0x406C2304):
        tgt = _decode_branch(va, rd('<I', va - BASE)[0])
        need(RELOC_HEIF_LO <= tgt < RELOC_HEIF_HI, f'HEIF hook {va:#x} -> {tgt:#x}')
    need(_decode_branch(0x410B7C64, rd('<I', 0x410B7C64 - BASE)[0]) == 0x40B13800, 'C-low formula dispatch')
    for state, lo, hi in ((0xAB, RELOC_HEIF_LO, RELOC_HEIF_HI), (0x17D, 0x40A00000, 0x40A20000)):
        pos = 0x4BFFCC0 + 96 * state
        cb, pg = rd('<Q', pos + 40)[0], rd('<Q', pos + 80)[0]
        need(lo <= cb < hi and lo <= pg < hi, f'submenu {state:#x} pointers {cb:#x}, {pg:#x}')

    # Relocated photo-areas checks
    need(rd('<Q', 0x4DA1288)[0] == RELOC_CLOW_DESC, 'relocated C-low geometry pointer')
    need(rd('<H', RELOC_CLOW_DESC - BASE + 10)[0] == 576, 'fixed crop height == 576')
    need(rd('<I', RELOC_AREA_MENU - BASE + 44 * 2)[0] == 3238, '4:3 crop label 3238')
    need(rd('<I', RELOC_AREA_MENU - BASE + 44 * 5)[0] == 3239, '2.39:1 crop label 3239')
    need(rd('<I', RELOC_AREA_MENU - BASE + 44 * 5 + 20)[0] == 0x7110, '2.39:1 menu icon 0x7110')
    need(rd('<I', RELOC_AREA_MENU - BASE + 44 * 6 + 20)[0] == 0x7111, '65:24 menu icon 0x7111')
    need(rd('<H', RELOC_AREA_ICONS - BASE + 0x140)[0] == 0x100, 'still icon descriptors')
    need(rd('<H', 0x4BD79D0 + 32)[0] == 7, 'crop menu row count == 7')
    need(rd('<H', 0x4B13000 + 0xA028C1690 - 0xA02808000)[0] == 11, 'video file type page: 11 rows')

    # 5. Relocated Q3_STASH RAM pointer & all zero-initialized private state pages
    need(rd('<Q', Q3_STASH_LITERAL_VA - BASE)[0] == RELOC_Q3_STASH_RAM, 'opengate Q3_STASH literal == 0xA0FF52000')
    for label, lo, hi in (
        ('q3_stash',    RELOC_Q3_STASH_VA, RELOC_Q3_STASH_VA + 0x1000),
        ('og_state',    0x4096F000, 0x40970000),
        ('ap_state',    0x4097F000, 0x40980000),
        ('pixelshift',  0x409ED000, 0x409F0000),
        ('stars_flag',  0x40A40000, 0x40A40001),
        ('log_priv',    0x40B0E000, 0x40B0F100),
    ):
        need(not any(image[lo - BASE:hi - BASE]), f'private zero state {label} ({lo:#x}..{hi:#x}) not zero')

    # 6. v0.5.1 framing-fixes unit, 4.1K 3:2 AF geometry/conversion entries, AF points, PLAYBACK_FIT & Pool #6 slot
    ff_off = FRAMING_FIXES_VA - BASE
    ff_sha = _sha(image[ff_off:ff_off + FRAMING_FIXES_BYTES])
    need(ff_sha == FRAMING_FIXES_SHA256, f'framing-fixes code SHA mismatch at 0x40968000: {ff_sha}')
    need(not any(image[ff_off + FRAMING_FIXES_BYTES:FRAMING_FIXES_CAVE_END - BASE]),
         'framing-fixes trailing cave 0x40968830..0x4096f000 not zero')

    af_geom_targets = {}
    for site in AF_GEOMETRY_CALLS:
        tgt = _decode_branch(site, rd('<I', site - BASE)[0], expect_link=True)
        need(tgt == AF_GEOMETRY_ENTRY, f'4.1K 3:2 AF geometry call at {site:#x} -> {tgt:#x} != {AF_GEOMETRY_ENTRY:#x}')
        af_geom_targets[hex(site)] = hex(tgt)

    fwd_tgt = _decode_branch(AF_FORWARD_SITE, rd('<I', AF_FORWARD_SITE - BASE)[0], expect_link=False)
    need(fwd_tgt == AF_FORWARD_ENTRY, f'4.1K 3:2 AF forward at 0x41055b38 -> {fwd_tgt:#x} != {AF_FORWARD_ENTRY:#x}')
    inv_tgt = _decode_branch(AF_INVERSE_SITE, rd('<I', AF_INVERSE_SITE - BASE)[0], expect_link=False)
    need(inv_tgt == AF_INVERSE_ENTRY, f'4.1K 3:2 AF inverse at 0x4105693c -> {inv_tgt:#x} != {AF_INVERSE_ENTRY:#x}')
    pts_tgt = _decode_branch(AP_POINTS_SCALE_SITE, rd('<I', AP_POINTS_SCALE_SITE - BASE)[0], expect_link=False)
    need(pts_tgt == POINTS_SOURCE_ENTRY, f'3:2 AF points at 0x409704b0 -> {pts_tgt:#x} != {POINTS_SOURCE_ENTRY:#x}')

    fit_tgt = _decode_branch(PLAYBACK_FIT_SITE, rd('<I', PLAYBACK_FIT_SITE - BASE)[0], expect_link=False)
    need(fit_tgt in (ASPECT_FIX_ENTRY, EVF_FIT_HOOK_ENTRY),
         f'PLAYBACK_FIT at 0x407138d0 -> {fit_tgt:#x} not in (aspect_fix 0x4019ffc0, evf_fit_hook 0x40968400)')

    pool6_slot = _decode_pool6_slot(image)
    need(pool6_slot in (POOL6_NFP_V051_SLOT, POOL6_V71_SUPERSET_SLOT),
         f'MOVS Pool #6 slot size {pool6_slot:#x} not in (0x18d800, 0x1c2000)')

    return dict(
        status='PASS',
        target_tag='v0.5.1',
        video_menu_rows=45,
        video_file_type_rows=11,
        crop_menu_rows=7,
        language_tables=36,
        offsets_per_language=STRING_ENTRIES,
        string_id_range=[3137, 3249],
        relocated_clow_desc=hex(RELOC_CLOW_DESC),
        relocated_heif_dispatch=hex(RELOC_HEIF_DISPATCH),
        relocated_area_menu=hex(RELOC_AREA_MENU),
        relocated_area_icons=hex(RELOC_AREA_ICONS),
        relocated_q3_stash_ram=hex(RELOC_Q3_STASH_RAM),
        loader_end=hex(loader_end),
        framing_fixes_code_va=hex(FRAMING_FIXES_VA),
        framing_fixes_code_bytes=FRAMING_FIXES_BYTES,
        framing_fixes_code_sha256=ff_sha,
        af_geometry_calls=af_geom_targets,
        af_forward_target=hex(fwd_tgt),
        af_inverse_target=hex(inv_tgt),
        ap_points_scale_target=hex(pts_tgt),
        playback_fit_target=hex(fit_tgt),
        pool6_slot_size=f'0x{pool6_slot:08x}',
        pool6_slot_superset_0x1c2000=(pool6_slot == POOL6_V71_SUPERSET_SLOT),
    )


def _detect_mode(image: bytearray, man: dict) -> str:
    """Classify pre-image cleanly among already_v051 / upgrade_from_v050 / upgrade_from_v040 / apply_on_stock."""
    # Find Region A anchor (0x40AF8000 heif-format, unique to relocated v0.5.0 / v0.5.1)
    # and framing-fixes anchor (0x40968000, unique to v0.5.1).
    heif_rec = next(r for r in man['apply_v050'] if r['offset'] == RELOC_HEIF_LO - BASE)
    ff_rec = next(r for r in man['apply_v050'] if r['offset'] == FRAMING_FIXES_VA - BASE)

    heif_cur = bytes(image[heif_rec['offset']:heif_rec['offset'] + heif_rec['size']])
    ff_cur = bytes(image[ff_rec['offset']:ff_rec['offset'] + ff_rec['size']])

    if _sha(heif_cur) == heif_rec['new_sha256']:
        if _sha(ff_cur) == ff_rec['new_sha256']:
            return 'already_v051'
        if not any(ff_cur):
            return 'upgrade_from_v050'
        raise ValueError(
            f'Relocated Region A matches v0.5.x, but 0x40968000..0x40968830 is neither zero nor v0.5.1 framing-fixes'
        )

    if any(heif_cur):
        raise ValueError(f'Unexpected non-zero data in Region A (0x40af8000): sha256={_sha(heif_cur)}')

    first = man['revert_v040'][0]
    first_slice = bytes(image[first['offset']:first['offset'] + first['size']])
    first_sha = _sha(first_slice)
    if first_sha == first['v040_sha256']:
        return 'upgrade_from_v040'
    if (first['stock_zero'] and not any(first_slice)) or (
        not first['stock_zero'] and first_sha == first['stock_sha256']
    ):
        return 'apply_on_stock'
    raise ValueError(
        f"Unrecognized pre-image at {first['va']}: sha256={first_sha} "
        f"(expected v0.4.0 {first['v040_sha256']} or stock)"
    )


def apply(image: bytearray, *, bundle_dir: Path | None = None) -> dict:
    """Atomically upgrade embedded NFP v0.4.0 or relocated v0.5.0 -> relocated full 4-group
    NFP v0.5.1 (or apply onto clean stock C3.11, or verify idempotently if already v0.5.1).

    Does NOT restamp the Bootguard Build ID at 0x401fe060; Stage 0 restamps
    after all Stage 0 patches complete.
    """
    if not isinstance(image, bytearray):
        raise TypeError('image must be a mutable bytearray')
    if len(image) != IMAGE_SIZE:
        raise ValueError(f'Unexpected image size: {len(image)} != {IMAGE_SIZE}')

    bdir = Path(bundle_dir) if bundle_dir is not None else (Path(__file__).resolve().parent / 'nfp_v050')
    man = json.loads((bdir / 'manifest.json').read_text(encoding='utf-8'))
    z_bytes = (bdir / man['bundle']['file']).read_bytes()
    if len(z_bytes) != man['bundle']['zlib_bytes'] or _sha(z_bytes) != man['bundle']['zlib_sha256']:
        raise ValueError('nfp_v050 bundle compressed integrity check failed')
    raw_blob = zlib.decompress(z_bytes)
    if len(raw_blob) != man['bundle']['raw_bytes'] or _sha(raw_blob) != man['bundle']['raw_sha256']:
        raise ValueError('nfp_v050 bundle decompressed integrity check failed')

    mode = _detect_mode(image, man)

    # Validate all 3 special sites BEFORE creating atomic scratch copy
    sp_aspect = man['special_sites']['aspect_fix_chain']
    aspect_off = sp_aspect['offset']
    aspect_cur = bytes(image[aspect_off:aspect_off + 4]).hex()
    allowed_aspect = {
        sp_aspect['stock_hex'],
        sp_aspect['v040_v050_nfp_hex'],
        sp_aspect['nfp_hex'],
        sp_aspect['aspect_fix_hex'],
    }
    if aspect_cur not in allowed_aspect:
        raise ValueError(f"Unexpected instruction at 0x407138d0: {aspect_cur} not in {sorted(allowed_aspect)}")

    sp_pool6 = man['special_sites']['pool6_slot_size']
    pool6_off = sp_pool6['offset']
    cur_pool6_slot = _decode_pool6_slot(image)
    if cur_pool6_slot not in (POOL6_STOCK_SLOT, POOL6_NFP_V051_SLOT, POOL6_V71_SUPERSET_SLOT):
        raise ValueError(f'Unexpected MOVS Pool #6 slot size at 0x40cf4620/30: {cur_pool6_slot:#x}')

    cur_loader_end = _decode_loader_end(image)
    if cur_loader_end != 0x408BF8F0 and not (0x408BF8F0 < cur_loader_end <= 0x40C00000 and cur_loader_end % 4096 == 0):
        raise ValueError(f'Unexpected loader_copy_end at 0x41184d8c: {cur_loader_end:#x}')

    # Snapshot forbidden banks to prove 0-byte unintended mutation
    forbidden_before = {name: _sha(image[lo - BASE:hi - BASE]) for name, lo, hi in FORBIDDEN_BANKS}

    # Perform atomic revert + apply on scratch copy
    work = bytearray(image)
    revert_rows = man['revert_v040']
    reverted_bytes = 0
    reverted_windows = 0

    if mode in ('upgrade_from_v040', 'apply_on_stock'):
        for rec in revert_rows:
            off, sz = rec['offset'], rec['size']
            cur = work[off:off + sz]
            if mode == 'upgrade_from_v040':
                if _sha(cur) != rec['v040_sha256']:
                    raise ValueError(f"v0.4.0 pre-image mismatch at {rec['va']} ({sz} B, {rec['purpose']})")
                if rec['stock_zero']:
                    work[off:off + sz] = bytes(sz)
                elif 'stock_hex' in rec:
                    work[off:off + sz] = bytes.fromhex(rec['stock_hex'])
                else:
                    bo = rec['stock_blob_off']
                    work[off:off + sz] = raw_blob[bo:bo + sz]
                reverted_bytes += sz
                reverted_windows += 1
            else:
                if rec['stock_zero']:
                    if any(cur):
                        raise ValueError(f"Expected zero stock cave at {rec['va']} ({sz} B)")
                else:
                    if _sha(cur) != rec['stock_sha256']:
                        raise ValueError(f"Stock pre-image mismatch at {rec['va']} ({sz} B, {rec['purpose']})")

    # Apply / verify all 1,320 non-special relocated NFP v0.5.1 windows
    applied_bytes = 0
    applied_windows = 0
    for rec in man['apply_v050']:
        off, sz = rec['offset'], rec['size']
        va_lo, va_hi = BASE + off, BASE + off + sz
        for fname, flo, fhi in FORBIDDEN_BANKS:
            if va_lo < fhi and flo < va_hi:
                raise ValueError(f"Apply window {va_lo:#x}..{va_hi:#x} intersects forbidden bank {fname}")

        cur = work[off:off + sz]
        bo = rec['blob_off']
        payload = raw_blob[bo:bo + sz]
        if _sha(payload) != rec['new_sha256']:
            raise ValueError(f"Payload SHA mismatch at {rec['va']}")

        if mode == 'already_v051':
            if _sha(cur) != rec['new_sha256']:
                raise ValueError(f"already_v051 mismatch at {rec['va']} ({sz} B, {rec['purpose']})")
            continue

        if mode == 'upgrade_from_v050':
            if 'v050_sha256' in rec:
                if rec.get('v050_zero', False):
                    if any(cur):
                        raise ValueError(f"Expected zero v0.5.0 cave before v0.5.1 delta at {rec['va']} ({sz} B)")
                elif _sha(cur) != rec['v050_sha256']:
                    raise ValueError(f"v0.5.0 pre-image mismatch before v0.5.1 delta at {rec['va']} ({sz} B, {rec['purpose']})")
                work[off:off + sz] = payload
                applied_bytes += sz
                applied_windows += 1
            else:
                if _sha(cur) != rec['new_sha256']:
                    raise ValueError(f"v0.5.0 unchanged window mismatch at {rec['va']} ({sz} B, {rec['purpose']})")
            continue

        # mode in ('upgrade_from_v040', 'apply_on_stock')
        if rec['expect_zero']:
            if any(cur):
                raise ValueError(f"Target cave not zero before v0.5.1 apply at {rec['va']} ({sz} B, {rec['purpose']})")
        else:
            if _sha(cur) != rec['expect_sha256']:
                raise ValueError(f"Stock pre-image mismatch before v0.5.1 apply at {rec['va']} ({sz} B, {rec['purpose']})")
        work[off:off + sz] = payload
        applied_bytes += sz
        applied_windows += 1

    # Special site 1: 0x407138d0 (commutative with Stage 0 aspect_fix)
    aspect_preserved = (aspect_cur == sp_aspect['aspect_fix_hex'])
    if not aspect_preserved and aspect_cur != sp_aspect['nfp_hex']:
        work[aspect_off:aspect_off + 4] = bytes.fromhex(sp_aspect['nfp_hex'])
        applied_bytes += 4
        applied_windows += 1

    # Special site 2: 0x40cf4620..0x40cf4634 (preserve Stage 0 5b superset 0x001C2000 >= 0x0018D800!)
    pool6_preserved = (cur_pool6_slot >= POOL6_NFP_V051_SLOT)
    if not pool6_preserved:
        work[pool6_off:pool6_off + 20] = bytes.fromhex(sp_pool6['nfp_v051_hex'])
        applied_bytes += 20
        applied_windows += 1
    new_pool6_slot = _decode_pool6_slot(work)
    if new_pool6_slot < cur_pool6_slot or new_pool6_slot < POOL6_NFP_V051_SLOT:
        raise ValueError(f'Pool #6 slot regression guard tripped: {cur_pool6_slot:#x} -> {new_pool6_slot:#x}')

    # Special site 3: 0x41184d8c loader_copy_end (preserve >= 0x40B50000, else raise to 0x40B50000)
    new_loader_end = max(cur_loader_end, 0x40B50000)
    if new_loader_end != cur_loader_end:
        work[0x41184D8C - BASE:0x41184D94 - BASE] = _encode_loader_end(new_loader_end)
        applied_bytes += 8
        applied_windows += 1

    # Post-mutation forbidden-bank preservation proof
    for name, lo, hi in FORBIDDEN_BANKS:
        if _sha(work[lo - BASE:hi - BASE]) != forbidden_before[name]:
            raise ValueError(f'Forbidden bank {name} ({lo:#x}..{hi:#x}) was unexpectedly modified!')

    # Full structural contract verification (incl. all v0.5.1 framing-fixes checks)
    contract = verify_contract(work)

    # Atomic commit
    image[:] = work

    return dict(
        schema='v71r5.patch_nfp_v051/v1',
        mode=mode,
        upstream=man['upstream'],
        framing_fixes=man['framing_fixes'],
        relocations=man['relocations'],
        reverted_v040_windows=reverted_windows,
        reverted_v040_bytes=reverted_bytes,
        applied_v050_windows=applied_windows,
        applied_v050_bytes=applied_bytes,
        total_non_special_windows=len(man['apply_v050']),
        aspect_fix_0x407138d0=dict(
            before=aspect_cur,
            after=bytes(image[aspect_off:aspect_off + 4]).hex(),
            preserved_aspect_fix=aspect_preserved,
        ),
        pool6_slot_0x40cf4620=dict(
            before=f'0x{cur_pool6_slot:08x}',
            after=f'0x{new_pool6_slot:08x}',
            preserved_existing=pool6_preserved,
            preserved_superset_0x1c2000=(new_pool6_slot == POOL6_V71_SUPERSET_SLOT),
        ),
        loader_copy_end_0x41184d8c=dict(
            before=hex(cur_loader_end),
            after=hex(new_loader_end),
            preserved_existing=(new_loader_end == cur_loader_end),
        ),
        forbidden_banks_unchanged=[name for name, _, _ in FORBIDDEN_BANKS],
        contract=contract,
    )


def restamp_build_id(image: bytearray) -> int:
    off = BUILD_ID_ADDR - BASE
    struct.pack_into('<I', image, off, 0)
    build_id = struct.unpack('<I', hashlib.sha256(image).digest()[:4])[0] | 0x01000000
    struct.pack_into('<I', image, off, build_id)
    return build_id


def main() -> None:
    ap = argparse.ArgumentParser(description='Upgrade embedded NFP v0.4.0 / v0.5.0 to relocated NFP v0.5.1 on a Z8 C3.11 MAIN image')
    ap.add_argument('--in', dest='inp', required=True, type=Path)
    ap.add_argument('--out', dest='out', required=True, type=Path)
    ap.add_argument('--bundle-dir', type=Path, default=None)
    ap.add_argument('--restamp-build-id', action='store_true')
    ap.add_argument('--meta-out', type=Path, default=None)
    args = ap.parse_args()

    img = bytearray(args.inp.read_bytes())
    info = apply(img, bundle_dir=args.bundle_dir)
    if args.restamp_build_id:
        info['build_id'] = hex(restamp_build_id(img))
    info['out_sha256'] = _sha(img)
    args.out.write_bytes(img)
    if args.meta_out:
        args.meta_out.write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
