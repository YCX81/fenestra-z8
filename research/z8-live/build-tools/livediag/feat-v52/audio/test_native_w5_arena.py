#!/usr/bin/env python3
"""Original ARM64 arena-save/copy/common-buffer blocks, no VPU operations."""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import *
H=Path(__file__).resolve().parent
D=(H.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes()
B=0x40160000;V=0xa02d3d928;POOL=0xa02d41848;S=0x4e010000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D)
u.mem_map(0xa02d3d000,0x10000);u.mem_map(0x4e000000,0x20000)
u.reg_write(UC_ARM64_REG_SP,S)
def w32(a,v):u.mem_write(a,struct.pack('<I',v))
def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
def r32(a):return struct.unpack('<I',u.mem_read(a,4))[0]
def r64(a):return struct.unpack('<Q',u.mem_read(a,8))[0]
rows=[]
for arena,size in [(0x8a2c9b000,0x1000000),(0x8b1357000,0x1800000)]:
 u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_X1,arena)
 u.reg_write(UC_ARM64_REG_W2,size);u.reg_write(UC_ARM64_REG_W3,8);u.reg_write(UC_ARM64_REG_W4,8)
 u.emu_start(0x40c3fae8,0x40c3fb10,count=100)
 assert r64(V+0x3f08)==arena and r32(V+0x3f10)==size
 u.reg_write(UC_ARM64_REG_X19,V)
 u.emu_start(0x40c3e9e8,0x40c3ea0c,count=100)
 assert r64(0xa02d3d950)==arena and r32(0xa02d3d948)==size
 # No common allocation or memcpy is faked: execute the native writes before
 # its memcpy call, then the first tracking-record writes before the logger.
 u.reg_write(UC_ARM64_REG_X1,POOL);u.reg_write(UC_ARM64_REG_X20,arena);u.reg_write(UC_ARM64_REG_X21,0)
 u.emu_start(0x40c3ee34,0x40c3ee4c,count=100)
 assert r32(POOL+0xc0)==0x200000 and r64(POOL+0xc8)==arena
 u.reg_write(UC_ARM64_REG_X8,V+0xa0)
 u.emu_start(0x40c3ee74,0x40c3ee98,count=100)
 assert r32(V+0x80)==0x200000 and r64(V+0x88)==arena and r32(V+0xa0)==1
 # Native allocator receives base + common_size, size - common_size.
 w64(V+0x10,POOL);u.reg_write(UC_ARM64_REG_X8,POOL)
 u.emu_start(0x40c3ea34,0x40c3ea4c,count=100)
 assert u.reg_read(UC_ARM64_REG_X1)==arena+0x200000 and u.reg_read(UC_ARM64_REG_W2)==size-0x200000
 rows.append(dict(arena=hex(arena),size=size,original_arena_field=hex(r64(V+0x28)),
                  common_size=r32(POOL+0xc0),allocator_start=hex(u.reg_read(UC_ARM64_REG_X1))))
out=dict(passed=True,firmware_sha256=hashlib.sha256(D).hexdigest(),vdi_struct=hex(V),arena_field=hex(V+0x28),size_field=hex(V+0x20),
         mistaken_field=hex(V+0xb0),mistaken_field_semantics='second 40-byte DMA record physical address, not arena identity',cases=rows,
         limitation='Native write/parameter instructions executed; no allocator lifetime, VPU or hardware behavior simulated.')
(H/'native-w5-arena.json').write_text(json.dumps(out,indent=2)+'\n')
print('PASS native arena field a02d3d950, size a02d3d948; common2MiB then allocator starts arena+2MiB')
