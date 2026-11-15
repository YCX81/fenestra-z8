#!/usr/bin/env python3
"""Actual G priority2 mailbox insertion/removal, no synthetic FIFO algorithm."""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;D=(P.parent/'v52g.main').read_bytes();B=0x40160000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D)
for a,n in [(0x4e000000,0x100000),(0xa02aaa000,0x1000),(0xa049b2000,0x1000),(0xa04b70000,0x1000)]:u.mem_map(a,n)
END,SP,TLS,TASK,BOX,DESC,OUT=0x4e001000,0x4e03f000,0x4e010000,0x4e011000,0x4e012000,0x4e013000,0x4e014000
w32=lambda a,v:u.mem_write(a,struct.pack('<I',v&0xffffffff))
w64=lambda a,v:u.mem_write(a,struct.pack('<Q',v))
r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0]
r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
def hook(uc,pc,size,data):
 if pc==0x40183490:
  assert uc.reg_read(UC_ARM64_REG_W0)==0,hex(uc.reg_read(UC_ARM64_REG_W0))
  uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,*args):
 u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_LR,END)
 for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
 u.emu_start(pc,END,count=20000);assert u.reg_read(UC_ARM64_REG_PC)==END,hex(u.reg_read(UC_ARM64_REG_PC))
 return u.reg_read(UC_ARM64_REG_W0)
u.reg_write(UC_ARM64_REG_TPIDR_EL1,TLS);u.reg_write(UC_ARM64_REG_PSTATE,5)
w32(TLS+0x50,0);w32(TLS+0x14,1);w32(0xa02aaaaf8,1);w64(0xa049b2878,BOX)
w64(BOX,BOX);w64(BOX+8,BOX);w64(BOX+16,DESC)
w32(DESC,2);w32(DESC+4,3);w32(TASK+0x38,1)
msgs=[0x4e020000+i*0x100 for i in range(5)]
for i,m in enumerate(msgs):
 w32(m+32,[0x04100030,0x04100031,0x04100030,0x04100032,0x04100030][i])
 call(0x40c00bac,TASK,m);assert r32(m+8)==2 and r64(m+16)==m
chain=[];m=r64(BOX+24)
while m:chain.append(m);assert len(chain)<=len(msgs);m=r64(m)
assert chain==msgs
out=[]
for _ in msgs:
 assert call(0x40d2c170,TASK+0x38,OUT)==1;out.append(r64(OUT))
assert out==msgs and r64(BOX+24)==0
report=dict(passed=True,firmware_sha256=hashlib.sha256(D).hexdigest(),insert=chain,remove=out,all_priority=2,
 executed='40c00bac ->40d2c128 ->actual4017cc84 kernel priority list; actual40d2c170 ->4017cf00 nonempty receive',
 fixture='Initialized single mailbox with no blocked waiters, flags2/maxpriority3 from41653908, normal thread TLS. Only40183490 error wrapper is stubbed after asserting success.',
 boundary='Proves FIFO for equal-priority messages in this native queue implementation. Does not prove timing of concurrent senders or absence of commands already queued elsewhere.')
(P/'sound-mailbox.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS actual G priority2 mailbox:5 start/stop/control envelopes inserted+received in FIFO order')
