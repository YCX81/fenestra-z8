#!/usr/bin/env python3
"""Delivered G actual sound constructor/dispatcher: driver-state admission.
No firmware patch: CPU instruction emulator with queue/alloc observation stubs.
"""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;D=(P.parent/'v52g.main').read_bytes();B=0x40160000
assert hashlib.sha256(D).hexdigest()=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D)
for a,n in [(0x4e000000,0x200000),(0xa038fe000,0x2000),(0xa02d4c000,0x1000)]:u.mem_map(a,n)
END,STACK,HEAP,CALLBACK=0x4e001000,0x4e03f000,0x4e100000,0x4e002000
w32=lambda a,v:u.mem_write(a,struct.pack('<I',v&0xffffffff));r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0];r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
queued=[];callbacks=[];heap=HEAP;admitted=False;reads=[]
def ret(v=None):
 if v is not None:u.reg_write(UC_ARM64_REG_X0,v)
 u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(uc,pc,size,data):
 global heap,admitted
 if pc==0x40d2c408:
  n=u.reg_read(UC_ARM64_REG_W0);a=heap;heap=(heap+n+15)&~15;u.mem_write(a,bytes(n));ret(a)
 elif pc==0x40d2c448:ret()
 elif pc==0x40c00bac:queued.append(u.reg_read(UC_ARM64_REG_X1));ret(1)
 elif pc==CALLBACK:
  p=u.reg_read(UC_ARM64_REG_X1);callbacks.append({'event':hex(u.reg_read(UC_ARM64_REG_W0)),'id':r32(p),'result':r32(p+4)});ret()
 elif pc==0x40e34eb4:reads.append(r32(u.reg_read(UC_ARM64_REG_SP)+0x2c))
 elif pc==0x40e34f08:admitted=True;u.emu_stop()
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,*args):
 u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,END)
 for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
 u.emu_start(pc,END,count=20000)
 assert u.reg_read(UC_ARM64_REG_PC) in (END,0x40e34f08),hex(u.reg_read(UC_ARM64_REG_PC))
 return u.reg_read(UC_ARM64_REG_W0)
rows=[]
for tone in (1,2,12):
 for driver in range(7):
  u.mem_write(0xa038fe000,bytes(0x2000));u.mem_write(0xa02d4c000,bytes(0x1000))
  w32(0xa038fea44,3);w32(0xa02d4cb48,driver)
  queued.clear();callbacks.clear();reads.clear();heap=HEAP;admitted=False
  rc=call(0x40e312d8,tone,CALLBACK)
  assert rc==1 and len(queued)==1
  msg=queued[0];assert r32(msg+32)==0x04100030
  call(0x40e34b2c,msg)
  assert reads==[driver]
  assert admitted==(driver not in (0,4,5)),(tone,driver,callbacks)
  if not admitted:assert callbacks and callbacks[0]=={'event':'0x4102004','id':tone,'result':0},callbacks
  rows.append(dict(tone=tone,driver=driver,queued_event=hex(r32(msg+32)),getter_observed=reads[:],reached_sound_open_setup=admitted,callbacks=callbacks[:]))
report=dict(passed=True,firmware_sha256=hashlib.sha256(D).hexdigest(),cases=rows,executed='40e312d8 constructor ->40e3133c ->40e32e6c; captured queue message -> actual40e34b2c/40e34d94; actual40c80aac/40c83908 getter',boundary='Heap allocation and RTOS queue are stubs; callback is observed. Accepted path stops at40e34f08 before OPEN/config/DMA; rejected path runs through actual callback dispatch. This does not observe the user hardware firing branch.',conclusion='Driver 0,4,5 refuse valid tone requests before OPEN14; driver1,2,3,6 reach setup when other sound/music state is idle. No gain/mute changes or firmware patch are used.')
(P/'sound-admission.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS',len(rows),'actual G tone requests: driver0/4/5 reject; others reach OPEN setup')
