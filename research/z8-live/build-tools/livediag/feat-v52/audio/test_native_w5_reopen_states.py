#!/usr/bin/env python3
"""Execute unmodified Nikon state/return blocks; no device completion simulated."""
from pathlib import Path
import hashlib
import json
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

HERE = Path(__file__).resolve().parent
FW = HERE.parents[1] / 'feat-v50/integration-writer/v51d/q/main.main'
D = FW.read_bytes()
BASE, RAM, S = 0x40160000, 0x4e000000, 0x4e010000
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(BASE, (len(D) + 4095) & ~4095)
u.mem_write(BASE, D)
u.mem_map(RAM, 0x20000)
u.mem_map(0xa02d30000, 0x3000)
events = []
header_rc, wait_success = 0, 1

def w32(a, v): u.mem_write(a, struct.pack('<I', v & 0xffffffff))
def w64(a, v): u.mem_write(a, struct.pack('<Q', v))
def r32(a): return struct.unpack('<I', u.mem_read(a, 4))[0]
def hook(uc, pc, size, data):
    if pc == 0x40c35048:
        assert uc.reg_read(UC_ARM64_REG_W1) == 0x33
        uc.reg_write(UC_ARM64_REG_W0, header_rc)
    elif pc == 0x40d2bc8c:
        events.append(uc.reg_read(UC_ARM64_REG_W1))
    elif pc == 0x40d2bdac:
        assert uc.reg_read(UC_ARM64_REG_W1) == 0x20
        uc.reg_write(UC_ARM64_REG_W0, wait_success)
    else:
        return
    uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE, hook)

headers = []
for state, header_rc in [(1, 0), (2, 0), (2, 1), (2, 3), (2, 0x10)]:
    u.mem_write(RAM, bytes(0x20000))
    w32(RAM, state)
    w64(S + 0x978, RAM + 0x100)
    w64(S + 0x9b0, RAM + 0x110)
    w64(S + 0x9e8, RAM + 0x120)
    w64(RAM + 0x100, 0x89f801000)
    w32(RAM + 0x110, 1024 * 1024)
    w32(RAM + 0x120, 1)
    w64(RAM + 0x130, RAM + 0x140)
    w32(0xa02d32770, 0x1234)
    for reg, value in [(UC_ARM64_REG_SP, S), (UC_ARM64_REG_X23, RAM),
                       (UC_ARM64_REG_X28, RAM + 0x130),
                       (UC_ARM64_REG_X20, RAM + 0x200),
                       (UC_ARM64_REG_X21, 0), (UC_ARM64_REG_W27, 0x33)]:
        u.reg_write(reg, value)
    events.clear()
    u.emu_start(0x40c3b668, 0x40c3b710, count=1000)
    assert u.reg_read(UC_ARM64_REG_PC) == 0x40c3b710
    success = state == 2 and header_rc == 0
    assert r32(RAM) == (3 if success else state)
    assert r32(0xa02d32770) == (0x1234 if success else 0xffffffff)
    assert events == ([] if success else [0x80])
    headers.append(dict(state_before=state, command_return=header_rc,
                        state_after=r32(RAM), result_word=r32(0xa02d32770),
                        acknowledged_events=list(events)))

set_params = []
for stored, wait_success in [(0, 1), (0xffffffff, 1), (0xfffffffd, 1), (0, 0)]:
    w32(0xa02d32768, stored)
    w32(0xa02d32788, 0)  # Worker has completed init successfully.
    w64(S + 8, 0)
    u.reg_write(UC_ARM64_REG_SP, S)
    u.reg_write(UC_ARM64_REG_X19, RAM + 0x200)
    u.reg_write(UC_ARM64_REG_X20, 0)
    u.emu_start(0x40c38a28, 0x40c38a74, count=1000)
    result = u.reg_read(UC_ARM64_REG_W8)
    assert result == (stored if wait_success else 0xfffffffd)
    set_params.append(dict(wait_success=wait_success, stored_result=stored,
                           worker_init_result=0, service_return=result))

expected = [
    (0x40c38a24, 0x40c38a78, 'ec745fba5f917abe997250f80a18df4dd1ad5fb8c96b58a53d0f736c039639bc'),
    (0x40c38af8, 0x40c38bb4, '697362bc58e7149f68358b8e2f769e1ca55997a94ab4b81d2a1dd0baaebce3df'),
    (0x40c3b668, 0x40c3b710, '055ec86b59b0a9183f4a1356730a917ea707640d4d2373c02288ab8a833de797'),
    (0x40c3d898, 0x40c3d8bc, 'a4ce5c671257273a7a6022fe894660ab60d948b7a8508c2d9913344fc487986f'),
    (0x40c5a268, 0x40c5a43c, 'c2d3f1705fc86684392bcd068f5112cfb628bb923af936e29ed11ff5ba85ad7b'),
]
for a, b, sha in expected:
    assert hashlib.sha256(D[a-BASE:b-BASE]).hexdigest() == sha
report = dict(passed=True, tested_firmware_sha256=hashlib.sha256(D).hexdigest(),
    verified_same_blocks_in_v52eclose='3b976636e4b67c86a9e93b0d82c598d23a7ea3e73c4733b3a7b82b27f252e7cb',
    header_states=headers, set_params_return_slots=set_params,
    limitation='VPU command and event wait/post are test boundaries. This proves exact native branches, not which branch occurred in the Eclose full run.')
(HERE / 'native-w5-reopen-states.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS original ARM HEADERS state/result branches and SET_PARAMS distinct return slot')
