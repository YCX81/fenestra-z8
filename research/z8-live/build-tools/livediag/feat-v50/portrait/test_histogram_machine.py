#!/usr/bin/env python3
"""Execute stock/patched ARM64 histogram code with actual direction getter.

Unicorn is a CPU execution check, not a full camera or QEMU renderer test.
External settings/preset/set-rectangle services are observed deterministic stubs.
Caller-saved registers are deliberately clobbered by the settings services.
"""
import argparse
import itertools
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
from apply_portrait import BASE, HIST_CODE, STOCK_SHA, sha, need

START, FINISH = 0x4041b248, 0x4041b2d8
DIR = 0x4038d658
GET = 0x403ec398
PRESET = 0x403eb4a8
SET = 0x40c7b7c8
STACK, RECT = 0xa10000000, 0xa10001000
ASPECT, MODE = 0xa032cfc50, 0xa032d0669
SCREEN, POSE = 0xa03228308, 0xa03228244
INITIAL = (218, 448, 380, 530)


def execute(stock, patched, case):
    aspect, mode, preset, rotate, screen, pose, evf_choice = case
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(BASE, (len(stock)+4095)&-4096)
    uc.mem_write(BASE, stock)
    if patched:
        uc.mem_write(START, HIST_CODE)
    for address in {ASPECT & -4096, MODE & -4096, SCREEN & -4096,
                    STACK, RECT}:
        uc.mem_map(address, 4096)
    uc.mem_write(ASPECT+0x20, bytes([aspect]))
    uc.mem_write(ASPECT+0x13e, bytes([17]))
    uc.mem_write(MODE, bytes([mode]))
    uc.mem_write(SCREEN, struct.pack('<I', screen))
    uc.mem_write(POSE, struct.pack('<I', pose))
    uc.mem_write(RECT, struct.pack('<4h', *INITIAL))
    uc.reg_write(UC_ARM64_REG_SP, STACK+0xf00)
    uc.reg_write(UC_ARM64_REG_X9, ASPECT)
    uc.reg_write(UC_ARM64_REG_X11, aspect)
    uc.reg_write(UC_ARM64_REG_X20, RECT)
    uc.reg_write(UC_ARM64_REG_X21, MODE)
    calls = []
    set_calls = []

    def return_value(value):
        ret = uc.reg_read(UC_ARM64_REG_X30)
        for reg in range(UC_ARM64_REG_X0, UC_ARM64_REG_X18+1):
            uc.reg_write(reg, 0xdead0000 + reg)
        uc.reg_write(UC_ARM64_REG_X0, value)
        uc.reg_write(UC_ARM64_REG_PC, ret)

    def hook(uc, address, size, unused):
        if address == FINISH:
            uc.emu_stop()
        elif address == GET:
            prop = uc.reg_read(UC_ARM64_REG_X0)
            calls.append(('property', prop))
            need(prop in (0x2001dcf, 0x2001dce), f'unexpected property {prop:#x}')
            return_value(rotate if prop == 0x2001dcf else evf_choice)
        elif address == PRESET:
            args = tuple(uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                                  UC_ARM64_REG_X2, UC_ARM64_REG_X3))
            calls.append(('preset', args))
            need(args == (0x42ae701c, 4, 17, 0), f'preset args differ: {args}')
            return_value(preset)
        elif address == SET:
            at, x, y = (uc.reg_read(r) for r in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
            need(at == RECT, 'rectangle pointer clobbered')
            set_calls.append((at, x, y))
            # This service is checked by arguments; rectangle extent is irrelevant.
            uc.mem_write(at, struct.pack('<4h', x, y, x+162, y+82))
            return_value(0)

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(START, 0, count=1000)
    need(uc.reg_read(UC_ARM64_REG_PC) == FINISH, 'block did not reach stock continuation')
    need(uc.reg_read(UC_ARM64_REG_SP) == STACK+0xf00, 'stack not restored')
    return dict(rect=struct.unpack('<4h', uc.mem_read(RECT, 8)), sets=set_calls, calls=calls)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stock', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    a = p.parse_args()
    stock = a.stock.read_bytes()
    need(sha(stock) == STOCK_SHA, 'wrong stock image')
    checked = unchanged = portrait = 0
    # Four position indices, both effective portrait directions, and conditions
    # that disable effective rotation despite the physical orientation.
    for case in itertools.product((0, 1), (0, 1, 2, 3), range(4), (0, 1), (0, 1), range(4), (0, 1)):
        before = execute(stock, False, case)
        after = execute(stock, True, case)
        aspect, mode, preset, rotate, screen, pose, evf_choice = case
        effective = rotate != 0 and screen == 0 and pose in (1, 2)
        if aspect and effective:
            need(after['rect'] == INITIAL and not after['sets'], f'portrait preset not skipped: {case}')
            portrait += 1
        else:
            need(before['sets'] == after['sets'] and before['rect'] == after['rect'],
                 f'landscape/aspect path changed: {case}')
            unchanged += 1
        checked += 1
    report = dict(status='PASS', engine='Unicorn ARM64; not QEMU/full-camera validation',
                  stock_sha256=STOCK_SHA, histogram_code_sha256=sha(HIST_CODE),
                  cases=checked, preserved_stock_geometry=unchanged, skipped_portrait_preset=portrait,
                  external_services='stubbed settings, preset selector and rectangle setter; caller-saved register clobber',
                  effective_direction_getter='actual stock instructions at 0x4038d658',
                  physical_camera_verified=False)
    a.report.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
