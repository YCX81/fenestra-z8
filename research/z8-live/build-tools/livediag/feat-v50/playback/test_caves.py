#!/usr/bin/env python3
"""Execute the two assembled playback caves in Unicorn; no camera access."""
from pathlib import Path
import struct,json,hashlib
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
D=Path.home()/'fxfix/liveplay'
plan=(D/'liveplan-stock.bin').read_bytes();fbh=(D/'livefbh.bin').read_bytes()
REGS=[UC_ARM64_REG_X19+i for i in range(10)]
def emu(code,at,stop):
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 for addr,size in [(0x401fd000,0x1000),(0x402f3000,0x1000),(0x40cf9000,0x1000),(0xa045f0000,0x1000),(0xa02d30000,0x1000),(0x100000,0x1000)]:u.mem_map(addr,size)
 u.mem_write(at,code)
 for i,r in enumerate(REGS):u.reg_write(r,0x45670000+i)
 def end(uc,pc,size,data):
  if pc in stop:uc.emu_stop()
 u.hook_add(UC_HOOK_CODE,end)
 return u
def preserve(u):
 assert all(u.reg_read(r)==0x45670000+i for i,r in enumerate(REGS))
checks=[]
for label,codec,sizeclass,caller,ui_class,w,h,expected in [
 ('lcd',1,0,0x402f3e3c,9,1024,682,True),('evf',1,0,0x402f3e3c,9,1280,852,True),
 ('query',1,0,0x40cf99e4,9,1024,682,True),('fhd-boundary',1,0,0x402f3e3c,9,1920,1088,True),
 ('stock-size',1,0,0x402f3e3c,0,1024,682,False),('avc',0,0,0x402f3e3c,9,1024,682,False),
 ('other-sizeclass',1,1,0x402f3e3c,9,1024,682,False),('other-caller',1,0,0x402f3e40,9,1024,682,False),
 ('zero-width',1,0,0x402f3e3c,9,0,682,False),('zero-height',1,0,0x402f3e3c,9,1024,0,False),
 ('over-width',1,0,0x402f3e3c,9,1921,682,False),('over-height',1,0,0x402f3e3c,9,1024,1089,False)]:
 u=emu(plan,0x401fd000,{caller,0x402f3268});u.mem_write(0x100000,struct.pack('<III',codec,0x5566,sizeclass));u.mem_write(0x100100,b'\xa5'*36)
 u.mem_write(0xa045f0904,struct.pack('<HHI',w,h,ui_class));u.reg_write(UC_ARM64_REG_X0,0x100000);u.reg_write(UC_ARM64_REG_X1,0x100100);u.reg_write(UC_ARM64_REG_LR,caller)
 u.emu_start(0x401fd000,0,count=100);preserve(u)
 if expected:
  result=bytes(u.mem_read(0x100100,36));words=list(struct.unpack('<9I',result));assert words==[1,(w<<16)|8,h,30000,1001,15,1,0x2aea540,1],(label,words)
  assert u.reg_read(UC_ARM64_REG_PC)==caller and u.reg_read(UC_ARM64_REG_X0)==0x100100
 else:assert bytes(u.mem_read(0x100100,36))==b'\xa5'*36 and u.reg_read(UC_ARM64_REG_PC)==0x402f3268,label
 checks.append(label)
for h,reported,expected in [(682,688,688),(852,856,856),(852,864,864),(852,0,864),(852,848,864),(852,865,864),(1080,1088,1088)]:
 u=emu(fbh,0x401fd100,{0x402f3c58});u.reg_write(UC_ARM64_REG_LR,0x402f3c58);u.reg_write(UC_ARM64_REG_X0,1);u.reg_write(UC_ARM64_REG_X8,h+15)
 for r in [UC_ARM64_REG_X1,UC_ARM64_REG_X9,UC_ARM64_REG_X10,UC_ARM64_REG_X11]:u.reg_write(r,0x7654321)
 u.mem_write(0xa02d30164+32+8,struct.pack('<I',reported));u.emu_start(0x401fd100,0,count=100);preserve(u)
 assert u.reg_read(UC_ARM64_REG_X8)==expected
 assert all(u.reg_read(r)==0x7654321 for r in [UC_ARM64_REG_X1,UC_ARM64_REG_X9,UC_ARM64_REG_X10,UC_ARM64_REG_X11])
 checks.append(f'height-{h}-reported-{reported}')
print(json.dumps({'result':'passed','checks':checks,'plan_sha256':hashlib.sha256(plan).hexdigest(),'fbh_sha256':hashlib.sha256(fbh).hexdigest(),'scope':'actual cave instructions and preserved registers; no physical hardware or display proof'},indent=2))
