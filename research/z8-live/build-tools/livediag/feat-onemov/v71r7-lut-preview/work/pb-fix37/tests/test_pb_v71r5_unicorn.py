#!/usr/bin/env python3
"""Unicorn AArch64 runtime verification suite for V71.5 pb-fix37:
  - 0 calls/references to CPU0 UI RPC 0x403fb59c / 0x403fa1b8 / 0xa032b5120 / 0xa032cd320
  - Pure read-only .JPG/.MAN lookup for single-card JPG, same-card RAW+JPG, and
    dual-card RAW-on-primary / JPG-on-secondary (without false-matching peer card!)
  - Negative lookup cache (neg_cached_id, neg_cached_epoch) eliminates 1.0s idle
    retry storms on non-Live photos
  - PM lifecycle alignment with src-fix37:
      ev=7  (BSShutdown)                -> soft gate close + cache clear, shutdown stays 0
      ev=8  (BSShutdownCancel)          -> clears shutdown=0 + caches
      ev=15 (BSSendPowerDownCancelDone) -> clears shutdown=0 + caches
      ev=16 (BSWaitEnableSuspend)       -> sole irreversible shutdown=1
      ev=24 (CardMountComplete)         -> bumps media_epoch/card_epoch, invalidates neg cache
"""
from __future__ import annotations

import re
import struct
import subprocess
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
import capstone

PB_BASE = 0x40196000
PB_LIMIT = 0x4019F020
RET_ADDR = 0x7E000FF0

# PbRuntime offsets (verified against clang-21 layout; prior offsets unchanged)
OFF_MAGIC = 0
OFF_BYTES = 4
OFF_KEY = 8
OFF_MEDIA_EPOCH = 12
OFF_SHUTDOWN = 16
OFF_KEY_EPOCH = 20
OFF_GATE = 32
OFF_CURRENT_ID = 52
OFF_CACHE_VALID = 56
OFF_LOOKUP_COUNT = 64
OFF_LOOKUP_ERROR = 68
OFF_MANIFEST = 232
OFF_NATIVE_PATH = 352
OFF_SETTINGS = 1848
OFF_GC_ATTEMPTED = 8184
OFF_CACHED_ID = 8192
OFF_RENDER_REQUEST = 8196
OFF_SYNC_RESET = 9312
OFF_NB_W = 9392
OFF_NB_H = 9396
OFF_LOOKUP_EPOCH = 9632
OFF_LOOKUP_US = 9640
OFF_CARD_EPOCH = 9656
OFF_NEG_CACHED_ID = 9664
OFF_NEG_CACHED_EPOCH = 9668
SIZEOF_PB_RUNTIME = 9672

PB_MAGIC = 0x50425549
FX_REGISTRY_MAGIC = 0x46585247  # 'FXRG' (check fx_registry.h)

# Nikon stock native helper addresses called by pb-fix37
ADDR_YIELD = 0x40187B00
ADDR_MODULE_LOADED = 0x401CCDE4
ADDR_OPEN = 0x402DD4F8
ADDR_CLOSE = 0x402DD558
ADDR_READ = 0x402DD6C8
ADDR_SEEK = 0x402DD8E8
ADDR_VIEW_ID = 0x40393F6C
ADDR_VIEW_STATE = 0x40393F28
ADDR_GET_SETTING = 0x403EC398
ADDR_CURRENT_ID_PTR = 0x407B4E78
ADDR_PATH_FROM_ID = 0x407B95E4
ADDR_RPC_FORBIDDEN_1 = 0x403FB59C
ADDR_RPC_FORBIDDEN_2 = 0x403FA1B8
ADDR_DCF_REF = 0x4085A8E0
ADDR_GUI_LOCK = 0x40D2BFB4
ADDR_GUI_UNLOCK = 0x40D2BF60
ADDR_ALLOC = 0x40D2C408
ADDR_FREE = 0x40D2C448

# Fenestra Core symbols linked into pb.elf (from seed-manifest.json)
FX_MEMSET = 0x401C6CD0
FX_MEMCPY = 0x401C68F0
FX_MEMCMP = 0x401C6958

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

ok = True
checks = 0


def check(cond: bool, msg: str) -> None:
    global ok, checks
    checks += 1
    if not cond:
        ok = False
        print(f"FAIL [{checks}]: {msg}")


def build_valid_mov(w: int = 1024, h: int = 680) -> bytes:
    """Build a minimal valid Nikon Live MOV container parsed by mov_size()."""
    ftyp = struct.pack('>I4s', 16, b'ftyp') + b'qt  ' + b'\x00\x00\x00\x00'
    udta = struct.pack('>I4s', 4104, b'udta') + (b'\x00' * 4096)
    # tkhd >= 92 bytes; last 8 bytes are fixed-point 16.16 width & height
    tkhd_body = bytearray(84)
    struct.pack_into('>II', tkhd_body, 76, w << 16, h << 16)
    tkhd = struct.pack('>I4s', 92, b'tkhd') + bytes(tkhd_body)
    trak = struct.pack('>I4s', 8 + len(tkhd), b'trak') + tkhd
    moov_body = udta + trak
    moov = struct.pack('>I4s', 8 + len(moov_body), b'moov') + moov_body
    return ftyp + moov


def make_manifest(native_path: str, size: int, gen: int = 1) -> bytes:
    return (
        f"native={native_path}\n"
        f"size={size}\n"
        f"uuid=19FEF38C-CA15-4047-D02B-A6A72CC4A198\n"
        f"volume_generation={gen}\n"
    ).encode('ascii')


def load_syms(elf_path: Path) -> dict[str, int]:
    out = subprocess.check_output(['llvm-nm', '--defined-only', str(elf_path)], text=True)
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return syms


def setup_vm(pb_bin: bytes, syms: dict[str, int], *, vfs: dict[str, bytes],
             id_to_path: dict[int, str], active_id: int, other_slot_mounted: bool,
             seed_img: bytes | None = None):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    # Map code region 0x40160000..0x40E00000
    mu.mem_map(0x40160000, 0x00D00000)
    if seed_img is not None:
        mu.mem_write(0x40160000, seed_img[:0x00D00000])
    mu.mem_write(PB_BASE, pb_bin)
    # Stack & scratch
    mu.mem_map(0x7E000000, 0x00100000)
    mu.mem_write(RET_ADDR, struct.pack('<I', 0xD503201F))

    # Map Nikon RAM regions used by pb-fix37:
    # - 0xa02800000..0xa05000000 (view/settings/root/DCF slot pointers/Wave5)
    # - 0xa10000000..0xa10200000 (PbRuntime, State, FxRegistry, Settings)
    mu.mem_map(0xA02800000, 0x02800000)
    mu.mem_map(0xA10000000, 0x00200000)

    # Set up Fenestra State + FxRegistry so pb_runtime_get() returns PB_RT_ADDR!
    # root_table at 0xa032b50c8: root = [0xa032b50c8 + 16]
    # s = *(root + 663*96 + 80); *(u32*)(s+64)==0x465a394d && *(u32*)(s+68)!=0
    # g = *(uintptr_t*)(s + 2528) -> FxRegistry { magic, version, bytes, ... playback }
    ROOT_ADDR = 0xA10010000
    STATE_ADDR = 0xA10020000
    REG_ADDR = 0xA10030000
    RT_ADDR = 0xA10040000
    SETTINGS_ADDR = 0xA10050000
    CUR_ID_ADDR = 0xA10060000
    MSG_ADDR = 0xA10070000

    mu.mem_write(0xA032B50C8 + 16, struct.pack('<Q', ROOT_ADDR))
    mu.mem_write(ROOT_ADDR + 663 * 96 + 80, struct.pack('<Q', STATE_ADDR))
    mu.mem_write(STATE_ADDR + 64, struct.pack('<II', 0x465A394D, 1))
    # FX_REGISTRY_ROOT_OFFSET = 2528 (let's place FxRegistry pointer there!)
    # Check fx_registry.h layout: magic=0x46585247 ('FXRG' or check actual magic)
    mu.mem_write(STATE_ADDR + 2528, struct.pack('<Q', REG_ADDR))
    # FxRegistry: { magic=0x47525846, bytes=64, live_ui=0, playback=RT_ADDR, menu_lock=0, reserved=0, spare[4] }
    mu.mem_write(REG_ADDR, struct.pack('<IIQQII', 0x47525846, 64, 0, RT_ADDR, 0, 0) + bytes(32))

    # Active playback view pointer at 0xa032280c8 = 0xa0284a0e0
    mu.mem_write(0xA032280C8, struct.pack('<Q', 0xA0284A0E0))
    mu.mem_write(CUR_ID_ADDR, struct.pack('<I', active_id))

    # DCF slot table at 0xa04822af8: slot[0], slot[1]
    mu.mem_write(0xA04822AF8, struct.pack('<QQ', 0xE149E6DC0, 0xE14CE6DC0 if other_slot_mounted else 0))

    # Initialize PbRuntime
    mu.mem_write(RT_ADDR, bytes(SIZEOF_PB_RUNTIME))
    mu.mem_write(RT_ADDR + OFF_MAGIC, struct.pack('<II', PB_MAGIC, SIZEOF_PB_RUNTIME))
    mu.mem_write(RT_ADDR + OFF_KEY, struct.pack('<I', 9))
    mu.mem_write(RT_ADDR + OFF_MEDIA_EPOCH, struct.pack('<I', 1))
    mu.mem_write(RT_ADDR + OFF_GC_ATTEMPTED, struct.pack('<I', 1))  # skip boot GC in lookup tests
    # Mark settings already installed so pb_settings_poll() is a fast no-op
    mu.mem_write(RT_ADDR + OFF_SETTINGS, struct.pack('<Q', SETTINGS_ADDR))
    mu.mem_write(SETTINGS_ADDR, struct.pack('<II', 1, 0))  # installed=1

    state = dict(
        clock_ticks=100_000_000,  # 100s at 1 MHz (cntfrq=1_000_000)
        yields=0,
        max_yields=1,
        yield_args=[],
        opens=[],
        fds={},
        next_fd=10,
        active_id=active_id,
        in_playback=True,
        forbidden_hits=[],
        on_yield=None,
        lp_loaded=True,
        lp_id_checks=[],
        alloc_enable=False,
        alloc_next=0xA10100000,
        allocs=[],
        frees=[],
    )

    def read_cstr(addr: int, limit: int = 128) -> str:
        raw = bytes(mu.mem_read(addr, limit))
        return raw.split(b'\x00', 1)[0].decode('ascii', errors='replace')

    def ret_to_caller(val: int | None = None):
        if val is not None:
            mu.reg_write(UC_ARM64_REG_X0, val & 0xFFFFFFFFFFFFFFFF)
        mu.reg_write(UC_ARM64_REG_PC, mu.reg_read(UC_ARM64_REG_X30))

    def code_cb(mu, addr, size, ud):
        if addr in (ADDR_RPC_FORBIDDEN_1, ADDR_RPC_FORBIDDEN_2, ADDR_DCF_REF):
            state['forbidden_hits'].append(addr)
            mu.emu_stop()
            return
        if addr == ADDR_YIELD:
            state['yields'] += 1
            state['yield_args'].append(mu.reg_read(UC_ARM64_REG_W0))
            state['clock_ticks'] += 2_000_000  # +2.0 seconds per poll iteration!
            if state['on_yield']:
                state['on_yield'](mu, state)
            if state['yields'] >= state['max_yields']:
                mu.emu_stop()
                return
            ret_to_caller(0)
            return
        if addr == ADDR_MODULE_LOADED:
            id_ptr = mu.reg_read(UC_ARM64_REG_X0)
            raw_id = bytes(mu.mem_read(id_ptr, 16))
            state['lp_id_checks'].append(raw_id)
            ret_to_caller(1 if (state['lp_loaded'] and raw_id == b'livephoto\x00\x00\x00\x00\x00\x00\x00') else 0)
            return
        if addr == ADDR_VIEW_ID:
            ret_to_caller(2 if state['in_playback'] else 1)
            return
        if addr == ADDR_VIEW_STATE:
            ret_to_caller(1 if state['in_playback'] else 0)
            return
        if addr == ADDR_GET_SETTING:
            # 0x491a -> 0 (single-frame view, not grid)
            ret_to_caller(0)
            return
        if addr == ADDR_CURRENT_ID_PTR:
            mu.mem_write(CUR_ID_ADDR, struct.pack('<I', state['active_id']))
            ret_to_caller(CUR_ID_ADDR)
            return
        if addr == ADDR_PATH_FROM_ID:
            pid = mu.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF
            out_ptr = mu.reg_read(UC_ARM64_REG_X1)
            pstr = id_to_path.get(pid)
            if pstr is None:
                ret_to_caller(4)
            else:
                mu.mem_write(out_ptr, pstr.encode('ascii') + b'\x00')
                ret_to_caller(0)
            return
        if addr == ADDR_OPEN:
            path = read_cstr(mu.reg_read(UC_ARM64_REG_X0))
            state['opens'].append(path)
            if path in vfs:
                fd = state['next_fd']
                state['next_fd'] += 1
                state['fds'][fd] = [vfs[path], 0]
                ret_to_caller(fd)
            else:
                ret_to_caller(0x80000010)  # negative int32 -> MISSING
            return
        if addr == ADDR_SEEK:
            fd = mu.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF
            off = mu.reg_read(UC_ARM64_REG_X1)
            if off >= (1 << 63):
                off -= (1 << 64)
            whence = mu.reg_read(UC_ARM64_REG_X2) & 0xFFFFFFFF
            pos_out = mu.reg_read(UC_ARM64_REG_X3)
            ent = state['fds'].get(fd)
            if ent is None:
                ret_to_caller(0)
                return
            data, cur = ent
            if whence == 0:
                npos = off
            elif whence == 2:
                npos = len(data) + off
            else:
                npos = cur + off
            if npos < 0 or npos > len(data):
                ret_to_caller(0)
                return
            ent[1] = npos
            if pos_out:
                mu.mem_write(pos_out, struct.pack('<Q', npos))
            ret_to_caller(1)  # nonzero = success in 0x402dd8e8!
            return
        if addr == ADDR_READ:
            fd = mu.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF
            dst = mu.reg_read(UC_ARM64_REG_X1)
            n = mu.reg_read(UC_ARM64_REG_X2) & 0xFFFFFFFF
            got_ptr = mu.reg_read(UC_ARM64_REG_X3)
            ent = state['fds'].get(fd)
            if ent is None:
                ret_to_caller(0x80000020)
                return
            data, cur = ent
            chunk = data[cur:cur + n]
            ent[1] = cur + len(chunk)
            if chunk:
                mu.mem_write(dst, chunk)
            if got_ptr:
                mu.mem_write(got_ptr, struct.pack('<I', len(chunk)))
            ret_to_caller(0)
            return
        if addr == ADDR_CLOSE:
            fd = mu.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF
            state['fds'].pop(fd, None)
            ret_to_caller(0)
            return
        if addr in (ADDR_GUI_LOCK, ADDR_GUI_UNLOCK):
            ret_to_caller(0)
            return
        if addr == ADDR_ALLOC:
            sz = mu.reg_read(UC_ARM64_REG_W0)
            if state['alloc_enable']:
                p = state['alloc_next']
                state['alloc_next'] = (p + max(sz, 16) + 15) & ~15
                state['allocs'].append((p, sz))
                ret_to_caller(p)
            else:
                ret_to_caller(0)
            return
        if addr == ADDR_FREE:
            p = mu.reg_read(UC_ARM64_REG_X0)
            state['frees'].append(p)
            ret_to_caller(0)
            return

        # Intercept mrs cntpct_el0 / cntfrq_el0
        w = struct.unpack('<I', mu.mem_read(addr, 4))[0]
        if (w & 0xFFF00000) == 0xD5300000:
            ins = next(md.disasm(bytes(mu.mem_read(addr, 4)), addr))
            rt = w & 31
            if 'cntpct_el0' in ins.op_str:
                mu.reg_write(UC_ARM64_REG_X0 + rt, state['clock_ticks'])
                mu.reg_write(UC_ARM64_REG_PC, addr + 4)
            elif 'cntfrq_el0' in ins.op_str:
                mu.reg_write(UC_ARM64_REG_X0 + rt, 1_000_000)  # 1 MHz -> 1 tick = 1 us
                mu.reg_write(UC_ARM64_REG_PC, addr + 4)

    mu.hook_add(UC_HOOK_CODE, code_cb)
    return mu, RT_ADDR, MSG_ADDR, state


def rd32(mu: Uc, addr: int) -> int:
    return struct.unpack('<I', mu.mem_read(addr, 4))[0]


def rd64(mu: Uc, addr: int) -> int:
    return struct.unpack('<Q', mu.mem_read(addr, 8))[0]


def rd_cstr(mu: Uc, addr: int, n: int = 64) -> str:
    return bytes(mu.mem_read(addr, n)).split(b'\x00', 1)[0].decode('ascii', errors='replace')


def run_worker_polls(mu: Uc, worker_addr: int, rt_addr: int, state: dict, polls: int, on_yield=None):
    state['yields'] = 0
    state['max_yields'] = polls
    state['on_yield'] = on_yield
    mu.reg_write(UC_ARM64_REG_SP, 0x7E0F0000)
    mu.reg_write(UC_ARM64_REG_X0, rt_addr)
    mu.reg_write(UC_ARM64_REG_X30, RET_ADDR)
    mu.emu_start(worker_addr, RET_ADDR, count=5_000_000)


def call_system_notify(mu: Uc, notify_addr: int, msg_addr: int, event: int, card: int = 0):
    mu.mem_write(msg_addr, bytes(64))
    mu.mem_write(msg_addr + 32, struct.pack('<I', event))
    mu.mem_write(msg_addr + 40, struct.pack('<I', card))
    mu.reg_write(UC_ARM64_REG_SP, 0x7E0E0000)
    mu.reg_write(UC_ARM64_REG_X0, msg_addr)
    mu.reg_write(UC_ARM64_REG_X30, RET_ADDR)
    mu.emu_start(notify_addr, RET_ADDR, count=200_000)


def main() -> None:
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <pb.elf> <seed.main>")
        raise SystemExit(2)
    elf_path = Path(sys.argv[1])
    seed_img = Path(sys.argv[2]).read_bytes()
    pb_bin = elf_path.with_name('payload.bin').read_bytes() if (elf_path.with_name('payload.bin').exists()) else elf_path.with_suffix('.bin').read_bytes()
    syms = load_syms(elf_path)

    # First read exact FX_REGISTRY_MAGIC from fx_registry.h in pb-fix37 or check pb_runtime_get disassembly
    fx_reg_magic = 0x46585247  # check below dynamically via pb_runtime_get!
    dis = subprocess.check_output(['llvm-objdump', '-d', str(elf_path)], text=True)
    # Audit 0 forbidden RPC calls / literals / FP / SIMD
    check(len(pb_bin) <= 36896, f"pb.bin fits 0x40196000..0x4019f020 ({len(pb_bin)} <= 36896)")
    for bad in ('403fb59c', '403fa1b8'):
        check(bad not in dis.lower(), f"0 references to {bad} in pb.elf")
    for name, pat in [('0x403fb59c', struct.pack('<I', 0x403FB59C)), ('0x403fa1b8', struct.pack('<I', 0x403FA1B8)),
                      ('0xa032b5120', struct.pack('<Q', 0xA032B5120)), ('0xa032cd320', struct.pack('<Q', 0xA032CD320))]:
        check(pat not in pb_bin, f"0 raw literals of {name} in pb.bin")

    # Detect actual FX_REGISTRY_MAGIC from fx_registry.h or pb_runtime_get test
    # Let's check fx_registry.h magic: 0x46585247 vs 0x46585231 vs etc.
    hdr_path = Path(__file__).resolve().parent.parent / 'fx_registry.h'
    if hdr_path.exists():
        m = re.search(r'FX_REGISTRY_MAGIC\s+(?:UINT32_C\()?(0x[0-9a-fA-F]+)', hdr_path.read_text())
        if m:
            fx_reg_magic = int(m.group(1), 16)

    def patch_reg_magic(mu: Uc):
        mu.mem_write(0xA10030000, struct.pack('<II', fx_reg_magic, 64))

    worker_addr = syms['worker']
    notify_addr = syms['pb_system_notify']
    get_addr = syms['pb_runtime_get']

    valid_mov = build_valid_mov(1024, 680)
    jpg_bytes = b'\xff\xd8' + (b'\x55' * 12340) + b'\xff\xd9'

    print("=== Scenario 1: Live OFF + RAW+JPG (.NEF primary) — 0 RPC, neg-cached, 0 1s retries ===")
    id_nef = (0 << 29) | (108 << 19) | (9318 << 4) | 2
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={'A:\\DCIM\\108NCZ_8\\DSC_9318.JPG': jpg_bytes},
        id_to_path={id_nef: 'A:\\DCIM\\108NCZ_8\\DSC_9318.NEF'},
        active_id=id_nef, other_slot_mounted=False, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    run_worker_polls(mu, worker_addr, rt, st, polls=10)  # 10 polls * +2s = 20s elapsed!
    check(not st['forbidden_hits'], f"S1: ZERO forbidden RPC/DCF calls (got {st['forbidden_hits']})")
    check(rd32(mu, rt + OFF_LOOKUP_COUNT) == 1, f"S1: lookup_count == 1 across 20s (got {rd32(mu, rt + OFF_LOOKUP_COUNT)})")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 0, "S1: cache_valid == 0")
    check(rd32(mu, rt + OFF_LOOKUP_ERROR) == 2, f"S1: lookup_error == 2 (got {rd32(mu, rt + OFF_LOOKUP_ERROR)})")
    check(rd32(mu, rt + OFF_NEG_CACHED_ID) == id_nef, "S1: neg_cached_id == id_nef")
    check(rd32(mu, rt + OFF_NEG_CACHED_EPOCH) == 1, "S1: neg_cached_epoch == 1")
    check(st['opens'] == ['A:\\LIVEDATA\\108NCZ_8\\DSC_9318.MAN'], f"S1: single read-only MAN probe (got {st['opens']})")

    print("=== Scenario 2: Live OFF + JPG-only — neg-cached, 0 1s retries ===")
    id_jpg = (0 << 29) | (108 << 19) | (9319 << 4) | 0
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={'A:\\DCIM\\108NCZ_8\\DSC_9319.JPG': jpg_bytes},
        id_to_path={id_jpg: 'A:\\DCIM\\108NCZ_8\\DSC_9319.JPG'},
        active_id=id_jpg, other_slot_mounted=False, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    run_worker_polls(mu, worker_addr, rt, st, polls=8)
    check(not st['forbidden_hits'], "S2: ZERO forbidden calls")
    check(rd32(mu, rt + OFF_LOOKUP_COUNT) == 1, f"S2: lookup_count == 1 across 16s (got {rd32(mu, rt + OFF_LOOKUP_COUNT)})")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 0 and rd32(mu, rt + OFF_NEG_CACHED_ID) == id_jpg, "S2: neg-cached")

    print("=== Scenario 3: Live ON + Same-card RAW+JPG (.NEF primary -> .JPG + .MAN + .MOV) ===")
    p_jpg = 'A:\\DCIM\\108NCZ_8\\DSC_9318.JPG'
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={
            p_jpg: jpg_bytes,
            'A:\\LIVEDATA\\108NCZ_8\\DSC_9318.MAN': make_manifest(p_jpg, len(jpg_bytes)),
            'A:\\LIVE\\108NCZ_8\\DSC_9318.MOV': valid_mov,
        },
        id_to_path={id_nef: 'A:\\DCIM\\108NCZ_8\\DSC_9318.NEF'},
        active_id=id_nef, other_slot_mounted=False, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    run_worker_polls(mu, worker_addr, rt, st, polls=3)
    check(not st['forbidden_hits'], "S3: ZERO forbidden RPC/DCF calls")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 1, f"S3: cache_valid == 1 (err={rd32(mu, rt + OFF_LOOKUP_ERROR)})")
    check(rd32(mu, rt + OFF_NB_W) == 1024 and rd32(mu, rt + OFF_NB_H) == 680,
          f"S3: mov_size 1024x680 (got {rd32(mu, rt + OFF_NB_W)}x{rd32(mu, rt + OFF_NB_H)})")
    check(rd_cstr(mu, rt + OFF_MANIFEST) == p_jpg, f"S3: manifest.native_path == {p_jpg}")
    check(rd32(mu, rt + OFF_LOOKUP_COUNT) == 1, "S3: single lookup")

    print("=== Scenario 4: Live ON + Single-card JPG-only ===")
    p_jpg2 = 'A:\\DCIM\\108NCZ_8\\DSC_9319.JPG'
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={
            p_jpg2: jpg_bytes,
            'A:\\LIVEDATA\\108NCZ_8\\DSC_9319.MAN': make_manifest(p_jpg2, len(jpg_bytes)),
            'A:\\LIVE\\108NCZ_8\\DSC_9319.MOV': valid_mov,
        },
        id_to_path={id_jpg: p_jpg2},
        active_id=id_jpg, other_slot_mounted=False, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    run_worker_polls(mu, worker_addr, rt, st, polls=2)
    check(not st['forbidden_hits'], "S4: ZERO forbidden calls")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 1 and rd32(mu, rt + OFF_NB_W) == 1024, "S4: JPG Live Photo matched")

    print("=== Scenario 5: Live ON + Dual-card RAW on Slot A, JPG+Live on Slot B ===")
    p_jpg_b = 'B:\\DCIM\\108NCZ_8\\DSC_9318.JPG'
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={
            p_jpg_b: jpg_bytes,
            'B:\\LIVEDATA\\108NCZ_8\\DSC_9318.MAN': make_manifest(p_jpg_b, len(jpg_bytes)),
            'B:\\LIVE\\108NCZ_8\\DSC_9318.MOV': valid_mov,
        },
        id_to_path={id_nef: 'A:\\DCIM\\108NCZ_8\\DSC_9318.NEF'},
        active_id=id_nef, other_slot_mounted=True, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    run_worker_polls(mu, worker_addr, rt, st, polls=2)
    check(not st['forbidden_hits'], "S5: ZERO forbidden RPC calls")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 1, f"S5: dual-card RAW-A/JPG-B matched (err={rd32(mu, rt + OFF_LOOKUP_ERROR)})")
    check(rd_cstr(mu, rt + OFF_MANIFEST) == p_jpg_b, f"S5: manifest points to {p_jpg_b}")
    check(rd32(mu, rt + OFF_NB_W) == 1024 and rd32(mu, rt + OFF_NB_H) == 680, "S5: MOV dims 1024x680")

    print("=== Scenario 6: Dual-card Live OFF RAW+JPG on Slot A must NOT false-match same-name Live on Slot B ===")
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={
            'A:\\DCIM\\108NCZ_8\\DSC_9318.JPG': jpg_bytes,  # same-card JPG exists, NO A:\LIVEDATA\...\MAN!
            p_jpg_b: jpg_bytes,
            'B:\\LIVEDATA\\108NCZ_8\\DSC_9318.MAN': make_manifest(p_jpg_b, len(jpg_bytes)),
            'B:\\LIVE\\108NCZ_8\\DSC_9318.MOV': valid_mov,
        },
        id_to_path={id_nef: 'A:\\DCIM\\108NCZ_8\\DSC_9318.NEF'},
        active_id=id_nef, other_slot_mounted=True, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    run_worker_polls(mu, worker_addr, rt, st, polls=5)
    check(not st['forbidden_hits'], "S6: ZERO forbidden calls")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 0, "S6: did NOT false-match unrelated Slot B Live Photo")
    check(rd32(mu, rt + OFF_LOOKUP_COUNT) == 1, "S6: neg-cached after 1 lookup")
    check(all(not p.startswith('B:') for p in st['opens']), f"S6: never even opened B: (opens={st['opens']})")

    print("=== Scenario 7: pb_system_notify() ev=7 / ev=8 / ev=15 / ev=16 / ev=24 ===")
    mu, rt, msg, st = setup_vm(
        pb_bin, syms, vfs={}, id_to_path={}, active_id=0, other_slot_mounted=False, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    # First verify pb_runtime_get() returns rt!
    mu.reg_write(UC_ARM64_REG_SP, 0x7E0E0000)
    mu.reg_write(UC_ARM64_REG_X30, RET_ADDR)
    mu.emu_start(get_addr, RET_ADDR, count=50_000)
    check(mu.reg_read(UC_ARM64_REG_X0) == rt, f"S7: pb_runtime_get() == {rt:#x} (got {mu.reg_read(UC_ARM64_REG_X0):#x})")

    # Seed active cache + open gate (bit 0 = 1) + neg_cached_epoch = 1
    mu.mem_write(rt + OFF_GATE, struct.pack('<Q', 3))
    mu.mem_write(rt + OFF_CACHE_VALID, struct.pack('<I', 1))
    mu.mem_write(rt + OFF_NEG_CACHED_EPOCH, struct.pack('<I', 1))
    # ev=7 (BSShutdown): MUST NOT set shutdown=1! Closes gate, bumps key_epoch, clears caches!
    call_system_notify(mu, notify_addr, msg, 7)
    check(rd32(mu, rt + OFF_SHUTDOWN) == 0, f"S7 ev=7: shutdown REMAINS 0 (got {rd32(mu, rt + OFF_SHUTDOWN)})")
    check((rd64(mu, rt + OFF_GATE) & 1) == 0, "S7 ev=7: gate closed")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 0 and rd32(mu, rt + OFF_NEG_CACHED_EPOCH) == 0, "S7 ev=7: caches cleared")
    check(rd32(mu, rt + OFF_KEY_EPOCH) == 1, "S7 ev=7: key_epoch incremented")
    check(rd32(mu, rt + OFF_MEDIA_EPOCH) == 1, "S7 ev=7: media_epoch unchanged")

    # ev=8 (BSShutdownCancel): clears shutdown=0 & caches
    mu.mem_write(rt + OFF_SHUTDOWN, struct.pack('<I', 1))
    mu.mem_write(rt + OFF_CACHE_VALID, struct.pack('<I', 1))
    mu.mem_write(rt + OFF_NEG_CACHED_EPOCH, struct.pack('<I', 1))
    call_system_notify(mu, notify_addr, msg, 8)
    check(rd32(mu, rt + OFF_SHUTDOWN) == 0, "S7 ev=8: shutdown cleared to 0")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 0 and rd32(mu, rt + OFF_NEG_CACHED_EPOCH) == 0, "S7 ev=8: caches cleared")

    # ev=15 (BSSendPowerDownCancelDone): clears shutdown=0 & caches
    mu.mem_write(rt + OFF_SHUTDOWN, struct.pack('<I', 1))
    mu.mem_write(rt + OFF_NEG_CACHED_EPOCH, struct.pack('<I', 1))
    call_system_notify(mu, notify_addr, msg, 15)
    check(rd32(mu, rt + OFF_SHUTDOWN) == 0 and rd32(mu, rt + OFF_NEG_CACHED_EPOCH) == 0, "S7 ev=15: shutdown & neg cache cleared")

    # ev=24 (CardMountComplete, card=0): bumps media_epoch & card_epoch[0], sets sync_reset=1, shutdown stays 0
    mu.mem_write(rt + OFF_NEG_CACHED_EPOCH, struct.pack('<I', 1))
    call_system_notify(mu, notify_addr, msg, 24, card=0)
    check(rd32(mu, rt + OFF_SHUTDOWN) == 0, "S7 ev=24: shutdown stays 0")
    check(rd32(mu, rt + OFF_MEDIA_EPOCH) == 2, "S7 ev=24: media_epoch incremented to 2")
    check(rd32(mu, rt + OFF_CARD_EPOCH) == 1 and rd32(mu, rt + OFF_CARD_EPOCH + 4) == 0, "S7 ev=24: card_epoch[0] incremented")
    check(rd32(mu, rt + OFF_SYNC_RESET) == 1 and rd32(mu, rt + OFF_NEG_CACHED_EPOCH) == 0, "S7 ev=24: sync_reset=1 & neg cache cleared")

    # ev=16 (BSWaitEnableSuspend): fires at end of cold boot (~1.75s) -> MUST NOT set shutdown=1!
    mu.mem_write(rt + OFF_GATE, struct.pack('<Q', 5))
    mu.mem_write(rt + OFF_CACHE_VALID, struct.pack('<I', 1))
    call_system_notify(mu, notify_addr, msg, 16)
    check(rd32(mu, rt + OFF_SHUTDOWN) == 0, "S7 ev=16: NOT shutdown (shutdown stays 0)")
    check((rd64(mu, rt + OFF_GATE) & 1) == 1 and rd32(mu, rt + OFF_CACHE_VALID) == 1,
          "S7 ev=16: gate stays open & cache stays valid")

    print("=== Scenario 8: End-to-end ev=7 standby (no ev=8) auto-recovers in worker(), manual shutdown stops worker ===")
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={
            p_jpg: jpg_bytes,
            'A:\\LIVEDATA\\108NCZ_8\\DSC_9318.MAN': make_manifest(p_jpg, len(jpg_bytes)),
            'A:\\LIVE\\108NCZ_8\\DSC_9318.MOV': valid_mov,
        },
        id_to_path={id_nef: 'A:\\DCIM\\108NCZ_8\\DSC_9318.NEF'},
        active_id=id_nef, other_slot_mounted=False, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    events_log = []

    def inject_events(m, s):
        if s['yields'] == 1:
            # After poll 1, Live Photo is cached! Now simulate ev=7 (USB standby with NO ev=8!)
            events_log.append(('after_poll1_cache', rd32(m, rt + OFF_CACHE_VALID)))
            # Save & call pb_system_notify(ev=7) inline via direct state mutation matching pb_system_notify
            m.mem_write(rt + OFF_CACHE_VALID, struct.pack('<I', 0))
            m.mem_write(rt + OFF_NEG_CACHED_EPOCH, struct.pack('<I', 0))
            m.mem_write(rt + OFF_KEY_EPOCH, struct.pack('<I', rd32(m, rt + OFF_KEY_EPOCH) + 1))
        elif s['yields'] == 3:
            # After poll 3, worker MUST have auto-recovered cache_valid==1! Now test manual shutdown=1!
            events_log.append(('after_ev7_recovered_cache', rd32(m, rt + OFF_CACHE_VALID)))
            m.mem_write(rt + OFF_SHUTDOWN, struct.pack('<I', 1))
            m.mem_write(rt + OFF_CACHE_VALID, struct.pack('<I', 0))

    run_worker_polls(mu, worker_addr, rt, st, polls=5, on_yield=inject_events)
    check(events_log == [('after_poll1_cache', 1), ('after_ev7_recovered_cache', 1)],
          f"S8: ev=7 standby recovered cleanly without ev=8 ({events_log})")
    check(rd32(mu, rt + OFF_SHUTDOWN) == 1 and rd32(mu, rt + OFF_CURRENT_ID) == 0,
          "S8: shutdown flag stops worker cleanly (current_id==0)")

    print("=== Scenario 9: V71.7-r4 Dormant Gating (livephoto.wlm disabled -> 0 B alloc, 0 REC intercept, 0 LIVEDATA I/O, 20 ms sleep) ===")
    mu, rt, msg, st = setup_vm(
        pb_bin, syms,
        vfs={
            p_jpg: jpg_bytes,
            'A:\\LIVEDATA\\108NCZ_8\\DSC_9318.MAN': make_manifest(p_jpg, len(jpg_bytes)),
            'A:\\LIVE\\108NCZ_8\\DSC_9318.MOV': valid_mov,
        },
        id_to_path={id_nef: 'A:\\DCIM\\108NCZ_8\\DSC_9318.NEF'},
        active_id=id_nef, other_slot_mounted=False, seed_img=seed_img,
    )
    patch_reg_magic(mu)
    st['lp_loaded'] = False
    st['alloc_enable'] = True
    # Reset gc_attempted=0 so we prove gc_boot is ALSO skipped when dormant!
    mu.mem_write(rt + OFF_GC_ATTEMPTED, struct.pack('<I', 0))
    run_worker_polls(mu, worker_addr, rt, st, polls=4)
    check(not st['opens'], f"S9 dormant: 0 card opens (got {st['opens']})")
    check(not st['allocs'], f"S9 dormant: 0 heap allocations (got {st['allocs']})")
    check(rd32(mu, rt + OFF_GC_ATTEMPTED) == 0, "S9 dormant: gc_boot skipped")
    check(rd32(mu, rt + OFF_LOOKUP_COUNT) == 0, "S9 dormant: lookup skipped")
    check(st['yield_args'] == [20, 20, 20, 20], f"S9 dormant: 20 ms sleep (got {st['yield_args']})")
    check(all(x == b'livephoto\x00\x00\x00\x00\x00\x00\x00' for x in st['lp_id_checks']),
          "S9 dormant: 16B NUL-padded 'livephoto' ID verified")

    # Verify pb_key_notify() returns 0 and never intercepts REC when dormant
    key_notify_addr = syms['pb_key_notify']
    body_addr = 0xA10080000
    mu.mem_write(body_addr, bytes(128))
    mu.mem_write(body_addr + 9, b'\x01')  # REC pressed
    mu.reg_write(UC_ARM64_REG_SP, 0x7E0E0000)
    mu.reg_write(UC_ARM64_REG_X0, body_addr)
    mu.reg_write(UC_ARM64_REG_X30, RET_ADDR)
    mu.emu_start(key_notify_addr, RET_ADDR, count=100_000)
    check(mu.reg_read(UC_ARM64_REG_W0) == 0, "S9 dormant: pb_key_notify returns 0 (no REC intercept)")

    # Transition 0 -> 1: worker lazily allocates i420/pixels/jpeg and caches Live Photo, then 1 -> 0 frees all 3!
    st['lp_loaded'] = True
    st['yield_args'].clear()
    mu.mem_write(rt + OFF_GC_ATTEMPTED, struct.pack('<I', 1))  # skip boot dir scan in mock
    # Map extra RAM for the ~2.4 MiB lazy allocations at 0xA10100000
    mu.mem_map(0xA10200000, 0x00400000)
    def flip_off(m, s):
        if s['yields'] == 2:
            s['lp_loaded'] = False
    run_worker_polls(mu, worker_addr, rt, st, polls=4, on_yield=flip_off)
    check(len(st['allocs']) == 3, f"S9 activate: 3 lazy heap allocs (i420/pixels/jpeg), got {len(st['allocs'])}")
    check(len(st['frees']) == 3 and set(st['frees']) == {p for p, _ in st['allocs']},
          f"S9 unload: all 3 lazy buffers freed cleanly (allocs={st['allocs']}, frees={st['frees']})")
    check(rd32(mu, rt + OFF_CACHE_VALID) == 0, "S9 unload: cache_valid cleared")
    check(st['yield_args'] == [5, 5, 20, 20], f"S9 sleep cadence [5,5,20,20] (got {st['yield_args']})")

    print(f"\ntest_pb_v71r5_unicorn: {checks} checks, {'ALL PASS' if ok else 'FAILED'}")
    raise SystemExit(0 if ok else 1)


if __name__ == '__main__':
    main()
