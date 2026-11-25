#!/usr/bin/env python3
"""Stage 0 standalone patch module for V71.5:
  1A-i.  identity() (0x401d1958, module_session.c:50-58): mask DAIF (#0xf) and
         save/restore PAR_EL1 & DAIF around `at s1e1r/w` via 80-byte zero-cave
         stub at 0x401d2dc8..0x401d2e18, eradicating SMP/IRQ PAR_EL1 corruption
         causing boot FENESTRA PANIC ERROR 0x00000003 (FX_CAPABILITY).
  1A-ii. fx_z8_task_start() (0x401d02ec, task.c:17): pin Fenestra loader and
         .wlm workers to single core PRCID 3 / CPU2 (processor=3, affinity=4)
         via 12-byte zero-cave stub at 0x401d2e18..0x401d2e24.
         Verified against Z8 C3.11 343-task table (0x44e2c9e0..0x44e2ef64) and
         cre_tsk (0x40185130..0x4018514c: 1<=processor<=3, affinity&(1<<(p-1))):
         CPU2 has only 61 stock tasks (22 prio<28 DSP/CV offload stubs, 3 prio=28)
         vs CPU0 (115 tasks, 55 prio<28, 13 prio=28 GUI/OpenVG/Play/STG + IRQs)
         and CPU1 (167 tasks, 82 prio<28 HEVC/MOV/FILEWM/IMPRAW); custom CPU2
         tasks (filmsim/pb_worker) run at prio 30 (> .wlm prio 28) so .wlm
         workers preempt immediately with zero starvation and zero cross-core
         migration.
  1A-iii. EL0 wall-clock deadline budget relaxation (100 ms -> 1500 ms) across
         ALL 3 Fenestra Core sites (still << 10000 ms watchdog_interval):
           - 0x401c4de0 (services.c:97):        descriptor.budget_ms = 1500
           - 0x401d22a8..0x401d22ac (module_worker.c:179): validate 1..1500
           - 0x401d1e10 (module_session.c:197): validate 1..1500 in invoke()
  1A-iv. Single-plugin fault isolation (eliminate single-plugin cascade-kill):
           - 0x401cfa50 (menu.c:765, supervisor/loader): call 68-byte cave stub
             at 0x401d2e24..0x401d2e68 that latches first faulted plugin into
             s->fault_record without setting s->phase=5 or s->shutdown=1.
           - 0x401cf89c..0x401cf8a4 (menu.c:714, startup_health): treat an
             isolated FX_Z8_WORKER_PANIC (5) worker as non-fatal/non-pending so
             startup.phase never enters FX_STARTUP_FAULT (6) -> global kill.
           - 0x401cf974..0x401cf97c (menu.c:728, startup_stop): treat an
             already-closed FX_Z8_WORKER_PANIC (5) worker as QUIESCED (4) so
             subsequent clean shutdown still removes BOOT-Z8.FLAG.
  1A-v.  Reconnect pb_system_entry (0x40d60ecc) in Fenestra Core's system
         dispatch trampoline (0x401d6004: b 0x40d60ecc) so pb_system_notify()
         receives all PM (ev=7/8/15/16) and card lifecycle (ev=22..31) events.
  1B.    cardslot_v71r4 (0x401fe300..0x401fef00, strictly <= 3,072 B so Stage 3
         font_overlay 0x401fef00..0x401fff80 stays 100% zero): SMP-safe 3-state
         CAS init + bounded try-spinlock + 20 ms throttle + 8 s timeout +
         hardened hook_read() with post-DMA `dc ivac` and 64B-aligned exclusive
         stack bounce buffer for unaligned head/tail cache lines.

Public API for Stage 0 integration:
    apply(image: bytearray, *, cardslot_dir: Path | None = None) -> dict
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

BASE = 0x40160000
BUILD_ID_ADDR = 0x401FE060

# Zero-cave layout inside Fenestra Core padding (0x401d2dc8..0x401d3000 = 568 B free)
IDENTITY_CAVE_ADDR = 0x401D2DC8
IDENTITY_CAVE_SIZE = 80                        # 0x401d2dc8..0x401d2e18
TASK_AFFINITY_CAVE_ADDR = 0x401D2E18
TASK_AFFINITY_CAVE_SIZE = 12                   # 0x401d2e18..0x401d2e24
ISOLATE_FAULT_CAVE_ADDR = 0x401D2E24
ISOLATE_FAULT_CAVE_SIZE = 68                   # 0x401d2e24..0x401d2e68
TOTAL_CORE_CAVE_SIZE = IDENTITY_CAVE_SIZE + TASK_AFFINITY_CAVE_SIZE + ISOLATE_FAULT_CAVE_SIZE  # 160 B

# Patch 1A-i: identity() PAR_EL1 + DAIF hardening
IDENTITY_AT_R_HOOK = 0x401D199C
IDENTITY_AT_W_HOOK = 0x401D19C8
IDENTITY_RESUME = 0x401D19A8

IDENTITY_PRE_WORDS = {
    0x401D1998: 0x37080182,  # tbnz w2, #1, #0x401d19c8
    0x401D199C: 0xD5087801,  # at s1e1r, x1
    0x401D19A0: 0xD5033FDF,  # isb
    0x401D19A4: 0xD5387408,  # mrs x8, par_el1
    0x401D19A8: 0xF240011F,  # tst x8, #1
    0x401D19C8: 0xD5087821,  # at s1e1w, x1
    0x401D19CC: 0xD5033FDF,  # isb
    0x401D19D0: 0xD5387408,  # mrs x8, par_el1
    0x401D19D4: 0x17FFFFF5,  # b #0x401d19a8
}

# Patch 1A-ii: fx_z8_task_start() single-core CPU2 affinity pin (processor=3, affinity=4)
TASK_AFFINITY_ADDR = 0x401D02EC
TASK_AFFINITY_RESUME = 0x401D02F0
TASK_AFFINITY_OLD = 0xB26083E8  # mov x8, #-0xffffffff  (0xffffffff_00000001)
TASK_AFFINITY_STORE_ADDR = 0x401D0300
TASK_AFFINITY_STORE_EXPECTED = 0xF9001FE8  # str x8, [sp, #0x38]
TASK_AFFINITY_PRCID = 3         # CPU2 (PRCID 3)
TASK_AFFINITY_MASK = 1 << (TASK_AFFINITY_PRCID - 1)  # 0x4

# Patch 1A-iii: EL0 wall-clock budget relaxation (100 ms -> 1500 ms across all 3 sites)
EL0_BUDGET_MS = 1500  # 0x5dc (12-bit imm, well below 10000 ms watchdog_interval)
BUDGET_PRE_WORDS = {
    # services.c:97 (fx_modules_discover_accounted)
    0x401C4DE0: 0x52800C8A,  # mov w10, #0x64
    0x401C4DEC: 0xB901D3EA,  # str w10, [sp, #0x1d0]
    # module_worker.c:179 (fx_z8_worker_start)
    0x401D22A4: 0xB9406688,  # ldr w8, [x20, #0x64]
    0x401D22A8: 0x51019508,  # sub w8, w8, #0x65
    0x401D22AC: 0x3101911F,  # cmn w8, #0x64
    0x401D22B0: 0x54000403,  # b.lo #0x401d2330
    # module_session.c:197 (fx_z8_session_invoke)
    0x401D1E04: 0x51000428,  # sub w8, w1, #1
    0x401D1E10: 0x71018D1F,  # cmp w8, #0x63
    0x401D1E20: 0x54000368,  # b.hi #0x401d1e8c
}
BUDGET_PATCH_WORDS = {
    0x401C4DE0: 0x5280000A | (EL0_BUDGET_MS << 5),            # mov w10, #0x5dc (1500)
    0x401D22A8: 0x51000108 | ((EL0_BUDGET_MS + 1) << 10),     # sub w8, w8, #0x5dd (1501)
    0x401D22AC: 0x3100011F | (EL0_BUDGET_MS << 10),           # cmn w8, #0x5dc (1500)
    0x401D1E10: 0x7100011F | ((EL0_BUDGET_MS - 1) << 10),     # cmp w8, #0x5db (1499)
}

# Patch 1A-iv: Single-plugin fault isolation (loader + startup_health + startup_stop)
SUPERVISOR_FAULT_CALL = 0x401CFA50
STARTUP_HEALTH_PANIC_BLK = 0x401CF89C
STARTUP_HEALTH_RESUME = 0x401CF858
STARTUP_STOP_PANIC_BLK = 0x401CF974
STARTUP_STOP_RESUME = 0x401CF93C

ISOLATION_PRE_WORDS = {
    # loader() / supervisor_task (0x401cf994)
    0x401CFA2C: 0x94000CAE,  # bl #0x401d2ce4 (fx_z8_worker_phase)
    0x401CFA30: 0x7100141F,  # cmp w0, #5
    0x401CFA34: 0x54000101,  # b.ne #0x401cfa54
    0x401CFA48: 0x2A0003E1,  # mov w1, w0
    0x401CFA4C: 0xAA1403E0,  # mov x0, x20
    0x401CFA50: 0x97FFF27B,  # bl #0x401cc43c (fx_native_module_fault)
    # startup_health() (0x401cf800)
    0x401CF850: 0x7100141F,  # cmp w0, #5
    0x401CF854: 0x54000240,  # b.eq #0x401cf89c
    0x401CF89C: 0xF9400268,  # ldr x8, [x19]
    0x401CF8A0: 0xF9400BF5,  # ldr x21, [sp, #0x10]
    # startup_stop() (0x401cf8b4)
    0x401CF934: 0x7100141F,  # cmp w0, #5
    0x401CF938: 0x540001E0,  # b.eq #0x401cf974
    0x401CF974: 0xF9400268,  # ldr x8, [x19]
    0x401CF978: 0xA9424FF4,  # ldp x20, x19, [sp, #0x20]
}

# Patch 1A-v: Reconnect pb_system_entry (0x40d60ecc) from Fenestra Core trampoline 0x401d6004
SYS_TRAMP_PROLOGUE_ADDR = 0x401D6000
SYS_TRAMP_PROLOGUE_EXPECTED = 0xA9BC63F9  # stp x25, x24, [sp, #-0x40]!
SYS_TRAMP_HOOK_ADDR = 0x401D6004
SYS_TRAMP_HOOK_OLD = 0xA9015BF7           # stp x23, x22, [sp, #0x10]
PB_SYSTEM_HOOK_TARGET = 0x40D60ECC

# Patch 1B: cardslot_v71r4 cave & hooks
CARDSLOT_CAVE = 0x401FE300
CARDSLOT_CEILING = 0x401FEF00  # Stage 3 font_overlay starts at 0x401FEF00
FONT_OVERLAY_END = 0x401FFF80

# Expected pre-patch bytes at the 7 wrapper entries: either old V42/V68 branch OR stock C3.11 insn
EXPECTED_HOOK_PRE = {
    'open':    (0x402DD4F8, {'9083fc17', 'f60f1df8'}),
    'remove':  (0x402DDA4C, {'7682fc17', 'f40f1ef8'}),
    'rename':  (0x402DD9FC, {'be82fc17', 'f553bea9'}),
    'opendir': (0x402DDC6C, {'d882fc17', 'f553bea9'}),
    'mkdir':   (0x402DDBDC, {'3783fc17', 'f40f1ef8'}),
    'read':    (0x402DD6C8, {'b084fc17', 'f963bca9'}),
    'write':   (0x402DD78C, {'b484fc17', 'ff8301d1'}),
}


def _branch(src: int, dst: int) -> int:
    delta = dst - src
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), f'Branch out of range: {src:#x} -> {dst:#x}'
    return 0x14000000 | ((delta // 4) & 0x3FFFFFF)


def _call(src: int, dst: int) -> int:
    delta = dst - src
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), f'BL out of range: {src:#x} -> {dst:#x}'
    return 0x94000000 | ((delta // 4) & 0x3FFFFFF)


def _u32(image: bytearray, addr: int) -> int:
    return struct.unpack_from('<I', image, addr - BASE)[0]


def _put_u32(image: bytearray, addr: int, word: int) -> None:
    struct.pack_into('<I', image, addr - BASE, word)


def _build_identity_cave() -> bytes:
    r_base = IDENTITY_CAVE_ADDR
    w_base = IDENTITY_CAVE_ADDR + 40
    stub_r = [
        0xD53B422A,  # mrs x10, daif
        0xD5034FDF,  # msr daifset, #0xf
        0xD538740B,  # mrs x11, par_el1
        0xD5087801,  # at s1e1r, x1
        0xD5033FDF,  # isb
        0xD5387408,  # mrs x8, par_el1
        0xD518740B,  # msr par_el1, x11
        0xD51B422A,  # msr daif, x10
        0xD5033FDF,  # isb
        _branch(r_base + 36, IDENTITY_RESUME),
    ]
    stub_w = [
        0xD53B422A,  # mrs x10, daif
        0xD5034FDF,  # msr daifset, #0xf
        0xD538740B,  # mrs x11, par_el1
        0xD5087821,  # at s1e1w, x1
        0xD5033FDF,  # isb
        0xD5387408,  # mrs x8, par_el1
        0xD518740B,  # msr par_el1, x11
        0xD51B422A,  # msr daif, x10
        0xD5033FDF,  # isb
        _branch(w_base + 36, IDENTITY_RESUME),
    ]
    raw = b''.join(struct.pack('<I', w) for w in stub_r + stub_w)
    assert len(raw) == IDENTITY_CAVE_SIZE
    return raw


def _build_task_affinity_cave() -> bytes:
    """Build 12-byte stub setting x8 = (affinity << 32) | processor for CPU2 (PRCID 3, affinity 4)."""
    words = [
        0x52800008 | (TASK_AFFINITY_PRCID << 5),   # mov  w8, #3             (processor = 3 = CPU2)
        0xF2C00008 | (TASK_AFFINITY_MASK << 5),    # movk x8, #4, lsl #32    (affinity  = 4 = bit2)
        _branch(TASK_AFFINITY_CAVE_ADDR + 8, TASK_AFFINITY_RESUME),
    ]
    raw = b''.join(struct.pack('<I', w) for w in words)
    assert len(raw) == TASK_AFFINITY_CAVE_SIZE
    return raw


def _build_isolate_fault_cave() -> bytes:
    """Build 68-byte helper called via BL from supervisor_task (0x401cfa50).

    Inputs: x0 = FxPlatformOwner* (or NULL), w1 = FxResult error, x19 = State *s.
    Latches first isolated plugin fault ((owner->owner << 32) | (err ? err : 10))
    into s->fault_record (s + 0x1e7a0) via atomic CAS from 0.
    NEVER touches s->phase (stays 2=Active) or s->shutdown (stays 0), so all
    other healthy .wlm plugins remain 100% isolated and running.
    """
    words = [
        0x2A0103E1,  # +0x00: mov   w1, w1           (zero-extend x1[63:32]=0)
        0x35000041,  # +0x04: cbnz  w1, +8           (-> +0x0c)
        0x52800141,  # +0x08: mov   w1, #10          (FX_INTEGRITY if err==0)
        0xAA1F03E9,  # +0x0c: mov   x9, xzr          (owner_id = 0)
        0xB4000060,  # +0x10: cbz   x0, +12          (-> +0x1c)
        0xB9400809,  # +0x14: ldr   w9, [x0, #8]     (owner->owner)
        0xD3607D29,  # +0x18: lsl   x9, x9, #32      ((uint64_t)owner_id << 32)
        0xAA010129,  # +0x1c: orr   x9, x9, x1       (record = (owner_id<<32)|err)
        0x91407A6A,  # +0x20: add   x10, x19, #0x1e, lsl #12
        0x911E8148,  # +0x24: add   x8, x10, #0x7a0  (&s->fault_record)
        0xC85FFD0A,  # +0x28: ldaxr x10, [x8]
        0xB500008A,  # +0x2c: cbnz  x10, +16         (-> +0x3c clrex; ret)
        0xC80BFD09,  # +0x30: stlxr w11, x9, [x8]
        0x35FFFFAB,  # +0x34: cbnz  w11, -12         (-> +0x28)
        0xD65F03C0,  # +0x38: ret
        0xD5033F5F,  # +0x3c: clrex
        0xD65F03C0,  # +0x40: ret
    ]
    raw = b''.join(struct.pack('<I', w) for w in words)
    assert len(raw) == ISOLATE_FAULT_CAVE_SIZE
    return raw


def apply(image: bytearray, *, cardslot_dir: Path | None = None) -> dict:
    """Apply V71.5 Boot PANIC 03 + Fenestra Core A2 resilience (1500 ms EL0
    budget, CPU2 single-core pin, single-plugin fault isolation, pb_system
    trampoline reconnect) + hardened cardslot_v71r4 in-place to a Stage 0 base
    image bytearray.

    Does NOT restamp the Bootguard Build ID at 0x401fe060; Stage 0 restamps
    after all Stage 0 patches complete.
    """
    if not isinstance(image, bytearray):
        raise TypeError('image must be a mutable bytearray')
    if len(image) < (0x402DE000 - BASE):
        raise ValueError(f'image too small: {len(image)}')

    cdir = Path(cardslot_dir) if cardslot_dir is not None else (Path(__file__).resolve().parent / 'cardslot_v71r4')
    cave_path = cdir / 'cave.bin'
    man_path = cdir / 'manifest.json'
    cave = cave_path.read_bytes()
    man = json.loads(man_path.read_text())

    # 0. Validate cardslot artifact integrity & hard ceiling before font_overlay
    cave_sha = hashlib.sha256(cave).hexdigest()
    assert cave_sha == man['cave_sha256'], f"cardslot cave.bin SHA mismatch: {cave_sha} != {man['cave_sha256']}"
    assert int(man['cave_addr'], 16) == CARDSLOT_CAVE
    assert len(cave) == man['cave_bytes']
    assert CARDSLOT_CAVE + len(cave) <= CARDSLOT_CEILING, (
        f"cardslot cave overflows font_overlay: {CARDSLOT_CAVE + len(cave):#x} > {CARDSLOT_CEILING:#x}"
    )

    # Verify font_overlay zero bank is currently 100% zero (never touch 0x401fef00..0x401fff80)
    font_slice = slice(CARDSLOT_CEILING - BASE, FONT_OVERLAY_END - BASE)
    assert not any(image[font_slice]), 'font_overlay (0x401fef00..0x401fff80) is not zero before patch!'

    # Verify combined Fenestra Core zero cave (160 B: 0x401d2dc8..0x401d2e68)
    core_cave_slice = slice(IDENTITY_CAVE_ADDR - BASE, IDENTITY_CAVE_ADDR - BASE + TOTAL_CORE_CAVE_SIZE)
    assert not any(image[core_cave_slice]), (
        f'Fenestra Core zero cave {IDENTITY_CAVE_ADDR:#x}..{IDENTITY_CAVE_ADDR + TOTAL_CORE_CAVE_SIZE:#x} is NOT zero!'
    )

    # 1A-i. Verify identity() pre-patch words
    for addr, exp in IDENTITY_PRE_WORDS.items():
        got = _u32(image, addr)
        assert got == exp, f'identity() pre-patch mismatch at {addr:#x}: got {got:#010x}, expected {exp:#010x}'

    # 1A-ii. Verify fx_z8_task_start() pre-patch words
    got_aff = _u32(image, TASK_AFFINITY_ADDR)
    assert got_aff == TASK_AFFINITY_OLD, (
        f'fx_z8_task_start() affinity mismatch at {TASK_AFFINITY_ADDR:#x}: '
        f'got {got_aff:#010x}, expected {TASK_AFFINITY_OLD:#010x}'
    )
    got_str = _u32(image, TASK_AFFINITY_STORE_ADDR)
    assert got_str == TASK_AFFINITY_STORE_EXPECTED, (
        f'fx_z8_task_start() store mismatch at {TASK_AFFINITY_STORE_ADDR:#x}: '
        f'got {got_str:#010x}, expected {TASK_AFFINITY_STORE_EXPECTED:#010x}'
    )

    # 1A-iii. Verify EL0 budget pre-patch words across all 3 sites
    for addr, exp in BUDGET_PRE_WORDS.items():
        got = _u32(image, addr)
        assert got == exp, f'EL0 budget pre-patch mismatch at {addr:#x}: got {got:#010x}, expected {exp:#010x}'

    # 1A-iv. Verify single-plugin fault isolation pre-patch words across loader/startup_health/startup_stop
    for addr, exp in ISOLATION_PRE_WORDS.items():
        got = _u32(image, addr)
        assert got == exp, f'fault isolation pre-patch mismatch at {addr:#x}: got {got:#010x}, expected {exp:#010x}'

    # 1A-v. Verify Fenestra Core system trampoline 0x401d6000..0x401d6008
    got_pro = _u32(image, SYS_TRAMP_PROLOGUE_ADDR)
    assert got_pro == SYS_TRAMP_PROLOGUE_EXPECTED, (
        f'sys_tramp prologue mismatch at {SYS_TRAMP_PROLOGUE_ADDR:#x}: got {got_pro:#010x}'
    )
    got_tr = _u32(image, SYS_TRAMP_HOOK_ADDR)
    assert got_tr == SYS_TRAMP_HOOK_OLD, (
        f'sys_tramp hook mismatch at {SYS_TRAMP_HOOK_ADDR:#x}: got {got_tr:#010x}, expected {SYS_TRAMP_HOOK_OLD:#010x}'
    )

    # 1B. Verify all 7 cardslot hook entries match either old V42/V68 branch or stock C3.11 insn
    hook_pre_actual = {}
    for name, (entry, allowed) in EXPECTED_HOOK_PRE.items():
        got_hex = bytes(image[entry - BASE:entry - BASE + 4]).hex()
        assert got_hex in allowed, (
            f'cardslot hook {name} pre-patch mismatch at {entry:#x}: got {got_hex}, allowed {sorted(allowed)}'
        )
        assert int(man['hooks'][name]['entry'], 16) == entry
        hook_pre_actual[name] = got_hex

    # --- All pre-checks passed; apply patches atomically ---

    # Build and install combined 160 B Fenestra Core cave (1A-i + 1A-ii + 1A-iv)
    id_cave = _build_identity_cave()
    aff_cave = _build_task_affinity_cave()
    iso_cave = _build_isolate_fault_cave()
    combined_cave = id_cave + aff_cave + iso_cave
    assert len(combined_cave) == TOTAL_CORE_CAVE_SIZE
    image[core_cave_slice] = combined_cave

    # Apply 1A-i: identity() DAIF + PAR_EL1 hooks
    br_r = _branch(IDENTITY_AT_R_HOOK, IDENTITY_CAVE_ADDR)
    br_w = _branch(IDENTITY_AT_W_HOOK, IDENTITY_CAVE_ADDR + 40)
    _put_u32(image, IDENTITY_AT_R_HOOK, br_r)
    _put_u32(image, IDENTITY_AT_W_HOOK, br_w)

    # Apply 1A-ii: fx_z8_task_start() single-core CPU2 pin (processor=3, affinity=4)
    br_aff = _branch(TASK_AFFINITY_ADDR, TASK_AFFINITY_CAVE_ADDR)
    _put_u32(image, TASK_AFFINITY_ADDR, br_aff)

    # Apply 1A-iii: EL0 wall-clock budget 100 ms -> 1500 ms across all 3 sites
    for addr, new_w in BUDGET_PATCH_WORDS.items():
        _put_u32(image, addr, new_w)

    # Apply 1A-iv: Single-plugin fault isolation
    bl_iso = _call(SUPERVISOR_FAULT_CALL, ISOLATE_FAULT_CAVE_ADDR)
    _put_u32(image, SUPERVISOR_FAULT_CALL, bl_iso)

    health_mov = 0x52800040  # mov w0, #2 (treat isolated PANIC as ACTIVE/non-pending)
    health_br = _branch(STARTUP_HEALTH_PANIC_BLK + 4, STARTUP_HEALTH_RESUME)
    _put_u32(image, STARTUP_HEALTH_PANIC_BLK, health_mov)
    _put_u32(image, STARTUP_HEALTH_PANIC_BLK + 4, health_br)

    stop_mov = 0x52800080  # mov w0, #4 (treat isolated PANIC as QUIESCED)
    stop_br = _branch(STARTUP_STOP_PANIC_BLK + 4, STARTUP_STOP_RESUME)
    _put_u32(image, STARTUP_STOP_PANIC_BLK, stop_mov)
    _put_u32(image, STARTUP_STOP_PANIC_BLK + 4, stop_br)

    # Apply 1A-v: Reconnect pb_system_entry (0x40d60ecc) in 0x401d6004
    br_sys = _branch(SYS_TRAMP_HOOK_ADDR, PB_SYSTEM_HOOK_TARGET)
    _put_u32(image, SYS_TRAMP_HOOK_ADDR, br_sys)

    # Apply 1B: clear old cardslot region 0x401fe300..0x401fef00, write new cave.bin, patch 7 hooks
    image[CARDSLOT_CAVE - BASE:CARDSLOT_CEILING - BASE] = bytes(CARDSLOT_CEILING - CARDSLOT_CAVE)
    image[CARDSLOT_CAVE - BASE:CARDSLOT_CAVE - BASE + len(cave)] = cave
    hooks_applied = {}
    for name, (entry, _) in EXPECTED_HOOK_PRE.items():
        hinfo = man['hooks'][name]
        new_bytes = bytes.fromhex(hinfo['branch'])
        assert len(new_bytes) == 4
        image[entry - BASE:entry - BASE + 4] = new_bytes
        hooks_applied[name] = dict(
            entry=hex(entry),
            old_hex=hook_pre_actual[name],
            new_hex=hinfo['branch'],
            target=hinfo['target'],
        )

    # Post-patch invariants
    assert not any(image[font_slice]), 'font_overlay (0x401fef00..0x401fff80) corrupted!'
    assert bytes(image[core_cave_slice]) == combined_cave
    assert _u32(image, IDENTITY_AT_R_HOOK) == br_r
    assert _u32(image, IDENTITY_AT_W_HOOK) == br_w
    assert _u32(image, TASK_AFFINITY_ADDR) == br_aff
    for addr, new_w in BUDGET_PATCH_WORDS.items():
        assert _u32(image, addr) == new_w
    assert _u32(image, SUPERVISOR_FAULT_CALL) == bl_iso
    assert _u32(image, STARTUP_HEALTH_PANIC_BLK) == health_mov
    assert _u32(image, STARTUP_HEALTH_PANIC_BLK + 4) == health_br
    assert _u32(image, STARTUP_STOP_PANIC_BLK) == stop_mov
    assert _u32(image, STARTUP_STOP_PANIC_BLK + 4) == stop_br
    assert _u32(image, SYS_TRAMP_HOOK_ADDR) == br_sys
    assert bytes(image[CARDSLOT_CAVE - BASE:CARDSLOT_CAVE - BASE + len(cave)]) == cave

    return dict(
        schema='v71r5.patch_boot_panic03_cardslot/v2',
        identity_par_el1=dict(
            cave_addr=hex(IDENTITY_CAVE_ADDR),
            cave_bytes=IDENTITY_CAVE_SIZE,
            cave_end=hex(IDENTITY_CAVE_ADDR + IDENTITY_CAVE_SIZE),
            cave_sha256=hashlib.sha256(id_cave).hexdigest(),
            hooks=[
                dict(address=hex(IDENTITY_AT_R_HOOK), old_word=hex(IDENTITY_PRE_WORDS[IDENTITY_AT_R_HOOK]),
                     new_word=hex(br_r), target=hex(IDENTITY_CAVE_ADDR), kind='at_s1e1r'),
                dict(address=hex(IDENTITY_AT_W_HOOK), old_word=hex(IDENTITY_PRE_WORDS[IDENTITY_AT_W_HOOK]),
                     new_word=hex(br_w), target=hex(IDENTITY_CAVE_ADDR + 40), kind='at_s1e1w'),
            ],
        ),
        task_affinity_pin=dict(
            address=hex(TASK_AFFINITY_ADDR),
            old_word=hex(TASK_AFFINITY_OLD),
            new_word=hex(br_aff),
            cave_addr=hex(TASK_AFFINITY_CAVE_ADDR),
            cave_bytes=TASK_AFFINITY_CAVE_SIZE,
            cave_end=hex(TASK_AFFINITY_CAVE_ADDR + TASK_AFFINITY_CAVE_SIZE),
            processor=TASK_AFFINITY_PRCID,
            affinity=TASK_AFFINITY_MASK,
            old_asm='mov x8, #0xffffffff00000001  // processor=1 (CPU0), affinity=0xffffffff',
            new_asm='b 0x401d2e18 -> mov w8, #3; movk x8, #4, lsl #32  // processor=3 (CPU2), affinity=4 (pinned)',
            rtos_check='cre_tsk 0x40185130..0x4018514c: 1<=processor<=3 && (affinity & (1 << (processor - 1))) != 0',
            topology_basis='Z8 C3.11 343-task table: CPU0=115(55<28,13=28+IRQs), CPU1=167(82<28,5=28), CPU2=61(22<28,3=28; filmsim/pb prio=30 > wlm prio=28)',
        ),
        el0_budget_relax=dict(
            budget_ms_old=100,
            budget_ms_new=EL0_BUDGET_MS,
            watchdog_interval_ms=10000,
            sites=[
                dict(address='0x401c4de0', func='fx_modules_discover_accounted',
                     old_word=hex(BUDGET_PRE_WORDS[0x401C4DE0]), new_word=hex(BUDGET_PATCH_WORDS[0x401C4DE0]),
                     old_asm='mov w10, #100', new_asm=f'mov w10, #{EL0_BUDGET_MS}'),
                dict(address='0x401d22a8', func='fx_z8_worker_start',
                     old_word=hex(BUDGET_PRE_WORDS[0x401D22A8]), new_word=hex(BUDGET_PATCH_WORDS[0x401D22A8]),
                     old_asm='sub w8, w8, #101', new_asm=f'sub w8, w8, #{EL0_BUDGET_MS + 1}'),
                dict(address='0x401d22ac', func='fx_z8_worker_start',
                     old_word=hex(BUDGET_PRE_WORDS[0x401D22AC]), new_word=hex(BUDGET_PATCH_WORDS[0x401D22AC]),
                     old_asm='cmn w8, #100', new_asm=f'cmn w8, #{EL0_BUDGET_MS}'),
                dict(address='0x401d1e10', func='fx_z8_session_invoke',
                     old_word=hex(BUDGET_PRE_WORDS[0x401D1E10]), new_word=hex(BUDGET_PATCH_WORDS[0x401D1E10]),
                     old_asm='cmp w8, #99', new_asm=f'cmp w8, #{EL0_BUDGET_MS - 1}'),
            ],
        ),
        fault_isolation=dict(
            cave_addr=hex(ISOLATE_FAULT_CAVE_ADDR),
            cave_bytes=ISOLATE_FAULT_CAVE_SIZE,
            cave_end=hex(ISOLATE_FAULT_CAVE_ADDR + ISOLATE_FAULT_CAVE_SIZE),
            cave_sha256=hashlib.sha256(iso_cave).hexdigest(),
            sites=[
                dict(address=hex(SUPERVISOR_FAULT_CALL), func='loader/supervisor_task',
                     old_word=hex(ISOLATION_PRE_WORDS[SUPERVISOR_FAULT_CALL]), new_word=hex(bl_iso),
                     effect='record first fault in s->fault_record only; keep s->phase=2 and s->shutdown=0'),
                dict(address=hex(STARTUP_HEALTH_PANIC_BLK), func='startup_health',
                     old_words=[hex(ISOLATION_PRE_WORDS[STARTUP_HEALTH_PANIC_BLK]),
                                hex(ISOLATION_PRE_WORDS[STARTUP_HEALTH_PANIC_BLK + 4])],
                     new_words=[hex(health_mov), hex(health_br)],
                     effect='treat isolated PANIC(5) worker as non-fatal/non-pending so startup.phase stays RUNNING'),
                dict(address=hex(STARTUP_STOP_PANIC_BLK), func='startup_stop',
                     old_words=[hex(ISOLATION_PRE_WORDS[STARTUP_STOP_PANIC_BLK]),
                                hex(ISOLATION_PRE_WORDS[STARTUP_STOP_PANIC_BLK + 4])],
                     new_words=[hex(stop_mov), hex(stop_br)],
                     effect='treat isolated PANIC(5) worker as QUIESCED(4) so clean shutdown removes BOOT-Z8.FLAG'),
            ],
        ),
        pb_system_trampoline=dict(
            address=hex(SYS_TRAMP_HOOK_ADDR),
            old_word=hex(SYS_TRAMP_HOOK_OLD),
            new_word=hex(br_sys),
            target=hex(PB_SYSTEM_HOOK_TARGET),
            effect='reconnect 0x401d6004 -> 0x40d60ecc (pb_system_entry) so pb_system_notify receives ev=7/8/15/16/22..31',
        ),
        cardslot_v71r4=dict(
            cave_addr=hex(CARDSLOT_CAVE),
            cave_ceiling=hex(CARDSLOT_CEILING),
            cave_bytes=len(cave),
            cave_end=hex(CARDSLOT_CAVE + len(cave)),
            cave_free_before_font_overlay=(CARDSLOT_CEILING - CARDSLOT_CAVE) - len(cave),
            cave_sha256=cave_sha,
            compiler=man.get('compiler'),
            hooks=hooks_applied,
        ),
        font_overlay_zero_verified=True,
    )


def restamp_build_id(image: bytearray) -> int:
    off = BUILD_ID_ADDR - BASE
    struct.pack_into('<I', image, off, 0)
    build_id = struct.unpack('<I', hashlib.sha256(image).digest()[:4])[0] | 0x01000000
    struct.pack_into('<I', image, off, build_id)
    return build_id


def main() -> None:
    ap = argparse.ArgumentParser(description='Apply V71.5 Boot PANIC 03 + Fenestra A2 resilience + hardened cardslot patch to a Stage 0 MAIN image')
    ap.add_argument('--in', dest='inp', required=True, type=Path)
    ap.add_argument('--out', dest='out', required=True, type=Path)
    ap.add_argument('--cardslot-dir', type=Path, default=None)
    ap.add_argument('--restamp-build-id', action='store_true')
    ap.add_argument('--meta-out', type=Path, default=None)
    args = ap.parse_args()

    img = bytearray(args.inp.read_bytes())
    info = apply(img, cardslot_dir=args.cardslot_dir)
    if args.restamp_build_id:
        info['build_id'] = hex(restamp_build_id(img))
    info['out_sha256'] = hashlib.sha256(img).hexdigest()
    args.out.write_bytes(img)
    if args.meta_out:
        args.meta_out.write_text(json.dumps(info, indent=2) + '\n')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
