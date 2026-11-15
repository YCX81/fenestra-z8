#!/usr/bin/env python3
"""Execute original ARM64 codec-map/command blocks with Unicorn.

VDI register writes and pool lookup are captured test boundaries. This checks
software dispatch/values, not VPU EOS completion or camera resource release.
"""
from pathlib import Path
import hashlib
import json
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

HERE = Path(__file__).resolve().parent
FW = HERE.parents[1] / 'feat-v50/integration-writer/v51d/q/main.main'
D = FW.read_bytes()
BASE, RAM, END = 0x40160000, 0x4e000000, 0x4e01f000
P, I, S = RAM + 0x1000, RAM + 0x2000, RAM + 0x10000
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
u.mem_map(BASE, (len(D) + 4095) & ~4095)
u.mem_write(BASE, D)
u.mem_map(RAM, 0x20000)
u.mem_map(0xa02d30000, 0x2000)
u.mem_write(END, bytes.fromhex('c0035fd6'))
writes = []

def w32(a, v): u.mem_write(a, struct.pack('<I', v))
def w64(a, v): u.mem_write(a, struct.pack('<Q', v))
def r32(a): return struct.unpack('<I', u.mem_read(a, 4))[0]
def hook(uc, pc, size, data):
    if pc == 0x40c3ef90:
        writes.append([uc.reg_read(UC_ARM64_REG_X0), uc.reg_read(UC_ARM64_REG_W1), uc.reg_read(UC_ARM64_REG_W2)])
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
    elif pc == 0x40c3ed44:
        uc.reg_write(UC_ARM64_REG_X0, I)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE, hook)

def run(start, end=END):
    u.reg_write(UC_ARM64_REG_SP, S)
    u.reg_write(UC_ARM64_REG_LR, END)
    u.emu_start(start, end, count=10000)
    assert u.reg_read(UC_ARM64_REG_PC) == end

mapping = []
for nikon, standard, mode in [(0, 0, 3), (1, 12, 1)]:
    # Worker stores open_params codec standard; stack pointers are the aliases
    # created in its native prologue (a6c8..a6e0).
    w32(P + 0x800, nikon)
    w64(S + 0x708, P + 0x10)
    w64(S + 0x710, P + 0xc)
    u.reg_write(UC_ARM64_REG_X29, P + 0x800)
    u.reg_write(UC_ARM64_REG_W14, 1)
    run(0x40c3b2ec, 0x40c3b30c)
    assert r32(P + 0xc) == standard
    u.reg_write(UC_ARM64_REG_X20, P)
    u.reg_write(UC_ARM64_REG_X19, I)
    w32(I + 0xc, 0xffffffff)
    run(0x40c58060, 0x40c58090)
    assert r32(I + 0xc) == mode
    mapping.append(dict(nikon_codec=nikon, open_standard=standard, instance_codec_mode=mode))

commands = []
for mode in [1, 3]:
    w32(I, 1); w32(I + 4, 2); w32(I + 8, 0)
    w32(I + 0xc, mode); w32(I + 0x18, 0)
    u.reg_write(UC_ARM64_REG_X0, I)
    run(0x40c46b44)
    assert u.reg_read(UC_ARM64_REG_W0) == 0
    for command in [0x20, 0x100]:
        writes.clear()
        u.reg_write(UC_ARM64_REG_X0, I)
        u.reg_write(UC_ARM64_REG_W1, command)
        run(0x40c541e0)
        assert writes == [[0, 0x110, (mode << 16) | 2], [0, 0x70, 1],
                          [0, 0x100, command], [0, 0x38, 1]], writes
        commands.append(dict(instance_codec_mode=mode, command=command, vdi_writes=list(writes)))
    writes.clear(); w32(P + 0x44, 1)
    u.reg_write(UC_ARM64_REG_X20, P); u.reg_write(UC_ARM64_REG_X21, 0)
    run(0x40c59a88, 0x40c59ab0)
    assert writes == [[0, 0x144, 0xffffffff]]
    w32(0xa02d31068, 0)
    u.reg_write(UC_ARM64_REG_W0, 0)
    run(0x40c39194)
    assert r32(0xa02d31068) == 1 and u.reg_read(UC_ARM64_REG_W0) == 0

ranges = [(0x40c3b2ec, 0x40c3b30c), (0x40c58060, 0x40c58090),
          (0x40c46b44, 0x40c46bc4), (0x40c541e0, 0x40c54268),
          (0x40c59a88, 0x40c59ab0), (0x40c39194, 0x40c391b8)]
report = dict(passed=True, firmware_sha256=hashlib.sha256(D).hexdigest(), mapping=mapping,
              commands=commands, eos_source_index=0xffffffff,
              code_ranges=[dict(start=hex(a), end=hex(b), sha256=hashlib.sha256(D[a-BASE:b-BASE]).hexdigest()) for a,b in ranges],
              boundaries=['native pool lookup returns the fixture instance pool', 'VDI register write function captured without hardware'],
              limitation='Exact instruction blocks only; complete worker EOS/finish/DMA and physical resource release not proven.')
(HERE / 'native-w5-codec.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS native Nikon codec0/1 -> standard0/12 -> instance mode3/1; both valid; EOS -1; finish/picture command mode retained')
