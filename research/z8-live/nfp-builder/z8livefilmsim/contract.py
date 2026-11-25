"""Independent structural contract checks for the V71.7-r7 Live Photo & FilmSim suite."""
import struct
from .overlay import (
    BUILD_ID_AT,
    EG_BYTES,
    RUNTIME_ID_AT,
    VERSION_STR_AT,
    compute_build_id,
    load_meta,
    verify_fenestra_assets,
)

BASE = 0x40160000
NFP_RELOC_CEILING = 0x40B50000
PLAYBACK_FIT_SITE = 0x407138D0
PLAYBACK_FIT_TARGET = 0x4019FFC0
XM6_ELF0_OFF = 0x0151CAA0


def _need(ok, message):
    if not ok:
        raise ValueError('V71.7-r7 contract: ' + message)


def _branch_target(word, va):
    imm = ((word & 0x3FFFFFF) ^ 0x2000000) - 0x2000000
    return va + imm * 4


def verify_contract(old_eg, new_eg):
    """Verify key V71.7-r7 invariants on the generated EG payload."""
    _need(len(old_eg) == EG_BYTES and len(new_eg) == EG_BYTES, 'EG payload length mismatch')
    read = lambda buf, fmt, at: struct.unpack_from(fmt, buf, at)

    # 1. Loader copy end == 0x40C00000 (covers Stage 3 CJK ext 2 + all relocated NFP caves).
    lo_ld, hi_ld = read(new_eg, '<II', 0x1024D8C)
    loader_end = ((lo_ld >> 5) & 65535) | (((hi_ld >> 5) & 65535) << 16)
    _need(loader_end == 0x40C00000, f'loader_copy_end must be 0x40c00000, got {loader_end:#x}')

    # 2. Pool #6 movie/EVF output slot size == 0x001C2000 (strict superset of NFP v0.5.1 0x18D800).
    lo_p6 = read(new_eg, '<I', 0x40CF4620 - BASE)[0]
    hi_p6 = read(new_eg, '<I', 0x40CF4630 - BASE)[0]
    _need((lo_p6 & 0xFFE0001F) == 0x5280000D and (hi_p6 & 0xFFE0001F) == 0x72A0000D,
          'Pool #6 slot instruction shape')
    pool6_size = ((lo_p6 >> 5) & 65535) | (((hi_p6 >> 5) & 65535) << 16)
    _need(pool6_size == 0x001C2000, f'Pool #6 slot size must be 0x001c2000, got {pool6_size:#x}')

    # 3. Relocated NFP v0.5.1 pointers all sit strictly below 0x40B50000.
    clow_ptr = read(new_eg, '<Q', 0x4DA1288)[0]
    _need(clow_ptr == 0x40AF1040 and clow_ptr < NFP_RELOC_CEILING,
          f'relocated C-low geometry pointer mismatch: {clow_ptr:#x}')
    _need(read(new_eg, '<H', clow_ptr - BASE + 10)[0] == 576, 'relocated C-low fixed crop height')

    area_menu_va = 0x40B16000
    area_icons_va = 0x40B1A000
    _need(area_menu_va < NFP_RELOC_CEILING and area_icons_va < NFP_RELOC_CEILING, 'area VA ceiling')
    _need(read(new_eg, '<I', area_menu_va - BASE + 44 * 2)[0] == 3238 and
          read(new_eg, '<I', area_menu_va - BASE + 44 * 5)[0] == 3239,
          'relocated crop label references')
    for row, icon in ((5, 0x7110), (6, 0x7111)):
        _need(read(new_eg, '<I', area_menu_va - BASE + 44 * row + 20)[0] == icon,
              f'relocated still menu icon row {row}')
    _need(read(new_eg, '<H', area_icons_va - BASE + 0x140)[0] == 0x100,
          'relocated still icon descriptors')
    _need(read(new_eg, '<H', 0x4BD79D0 + 32)[0] == 7, 'crop menu row count')

    # Video menu & 4.1K 3:2 AF geometry hooks (relocated NFP v0.5.1).
    rows, events, count = read(new_eg, '<QQH', 0x4BCC2C0)
    _need((rows, events, count) == (0x40A30000, 0x40A31000, 45), 'video menu allocation/count')
    _need(read(new_eg, '<H', 0x4B13000 + 0xA028C1690 - 0xA02808000)[0] == 11,
          'video file type page: 11 rows')
    for site in (0x41055AC0, 0x410567DC):
        word = read(new_eg, '<I', site - BASE)[0]
        _need((word >> 26) == 0x25 and _branch_target(word, site) == 0x40968000,
              f'4.1K 3:2 AF geometry entry at {site:#x}')

    # 4. PLAYBACK_FIT branch at 0x407138D0 -> 0x4019FFC0.
    pb_word = read(new_eg, '<I', PLAYBACK_FIT_SITE - BASE)[0]
    _need((pb_word >> 26) == 0x05 and _branch_target(pb_word, PLAYBACK_FIT_SITE) == PLAYBACK_FIT_TARGET,
          f'PLAYBACK_FIT branch at {PLAYBACK_FIT_SITE:#x} must target {PLAYBACK_FIT_TARGET:#x}')

    # 5. Bootguard build_id validity + 5-byte version string at 0x0009B136.
    stored_bid = read(new_eg, '<I', BUILD_ID_AT)[0]
    expected_bid = compute_build_id(memoryview(new_eg)[:-2])
    _need((stored_bid & 0x01000000) != 0 and stored_bid == expected_bid,
          f'Bootguard build_id mismatch: stored={stored_bid:#x}, expected={expected_bid:#x}')
    identity = new_eg[RUNTIME_ID_AT:RUNTIME_ID_AT + 8]
    _need(new_eg[VERSION_STR_AT:VERSION_STR_AT + 5] == identity[:5],
          '0x0009B136 version string mismatch')

    # 6. XM6 Core 0 ELF0 magic, class, PHDR count, and Slots 39/40/41 entries.
    _need(new_eg[XM6_ELF0_OFF:XM6_ELF0_OFF + 4] == b'\x7fELF' and new_eg[XM6_ELF0_OFF + 4] == 1,
          'XM6 ELF0 header magic/class mismatch')
    phnum = read(new_eg, '<H', XM6_ELF0_OFF + 44)[0]
    _need(phnum == 16, f'XM6 ELF0 e_phnum must be 16, got {phnum}')
    s39, s40, s41 = read(new_eg, '<3I', 0x01528214)
    _need((s39, s40, s41) == (0x418C4580, 0x418C4580, 0x418C45E0),
          'XM6 ELF0 function table slots 39/40/41 mismatch')

    # 7. FENESTRA manifest hashes match packaged assets.
    meta = load_meta()
    fenestra = verify_fenestra_assets(meta)

    return dict(
        status='PASS',
        profile_id=meta['id'],
        loader_end=hex(loader_end),
        pool6_slot_size=hex(pool6_size),
        relocated_clow_ptr=hex(clow_ptr),
        playback_fit_target=hex(PLAYBACK_FIT_TARGET),
        build_id=f'0x{stored_bid:08x}',
        xm6_elf0_phnum=phnum,
        fenestra_files=len(fenestra),
    )
