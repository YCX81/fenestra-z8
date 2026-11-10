#!/usr/bin/env python3
"""Unicorn checks of real AArch64 UI overlay, with file/GUI/RTOS service doubles."""
from pathlib import Path
import struct,json
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent
S={p[2]:int(p[0],16) for line in (P/'test.symbols').read_text().splitlines() if len(p:=line.split())==3}
U=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
firmware=(Path.home()/'.local/share/nikon-z8-qemu/fenestra-z8/bundle/fix-20261104.main').read_bytes()
U.mem_map(0x40160000,(len(firmware)+4095)&~4095);U.mem_write(0x40160000,firmware)
U.mem_write(0x401e2000,(P/'test.bin').read_bytes())
for base,size in [(0xa032b0000,0x10000),(0xa03bd0000,0x10000),(0xa10000000,0x200000),(0xa20000000,0x100000)]:U.mem_map(base,size)
def w32(a,v):U.mem_write(a,struct.pack('<I',v))
def w64(a,v):U.mem_write(a,struct.pack('<Q',v))
def r32(a):return struct.unpack('<I',U.mem_read(a,4))[0]
def r64(a):return struct.unpack('<Q',U.mem_read(a,8))[0]
def cstr(a):
 b=bytearray()
 while (v:=bytes(U.mem_read(a+len(b),1)))!=b'\0':b+=v
 return b.decode()
ROOT=0xa10000000;STATE=ROOT+0x10000;CORE=ROOT+0x20000;HEAP=ROOT+0x80000;MSG=ROOT+0x70000
w64(0xa032b50c8+16,ROOT);w64(ROOT+663*96+80,STATE)
w32(STATE+64,0x465a394d);w32(STATE+68,1);U.mem_write(STATE+32,struct.pack('<H',46))
w32(CORE,0x70000002);w64(CORE+80,CORE+96);w64(CORE+96+8,CORE+160)
w64(CORE+96+16,CORE+500);w64(CORE+96+24,CORE+1296);U.mem_write(CORE+96+32,struct.pack('<H',7))
w32(CORE+500,3156);w32(CORE+500+6*44,3158)
for i in range(5):w64(CORE+1440+i*8,0x401ff100)
w64(CORE+40,CORE+1440)
files={};fds={};fdnext=1;status=0;boot=0;calls=[]
def hook(uc,address,size,context):
 global fdnext,status,boot
 x=[uc.reg_read(UC_ARM64_REG_X0+i) for i in range(5)]
 ret=None
 if address==S['live_old_system_dispatch']:ret=123
 elif address==S['live_old_menu_page']:ret=CORE if x[:2]==[2,0x7000] else 0
 elif address==0x40d2c408:ret=HEAP
 elif address==0x40d2c448:raise AssertionError('UI installation allocation failed')
 elif address==S['live_runtime_boot']:boot+=1;ret=0
 elif address==S['live_runtime_event']:status=0;ret=0
 elif address==S['live_status']:ret=status
 elif address==S['live_requested_set']:status=1 if x[0] else 0;ret=0
 elif address==0x402dd4f8:
  p=cstr(x[0]);mode=x[1]
  if mode==0 and p not in files:ret=0xffffffff80000010
  elif mode==0x101 and p in files:ret=0xffffffff80000002
  else:
   assert mode in (0,0x101,0x301),hex(mode)
   if mode:files[p]=bytearray()
   ret=fdnext;fdnext+=1;fds[ret]=[p,0]
 elif address==0x402dd6c8:
  f=fds[x[0]];data=files[f[0]][f[1]:f[1]+x[2]];U.mem_write(x[1],bytes(data));w32(x[3],len(data));f[1]+=len(data);ret=0
 elif address==0x402dd78c:
  f=fds[x[0]];data=bytes(U.mem_read(x[1],x[2]));files[f[0]][f[1]:f[1]+len(data)]=data;f[1]+=len(data);w32(x[3],len(data));ret=0
 elif address==0x402dd558:del fds[x[0]];ret=0
 elif address==S['fx_native_display_attach_global']:ret=1 # No live GUI in this cheap test.
 if ret is not None:
  calls.append(address);uc.reg_write(UC_ARM64_REG_X0,ret);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
U.hook_add(UC_HOOK_CODE,hook)
STOP=0x401fffc0
def call(name,*args):
 for i,v in enumerate(args):U.reg_write(UC_ARM64_REG_X0+i,v)
 U.reg_write(UC_ARM64_REG_SP,0xa200ff000);U.reg_write(UC_ARM64_REG_LR,STOP)
 U.emu_start(S[name] if isinstance(name,str) else name,STOP,count=500000)
 assert U.reg_read(UC_ARM64_REG_PC)==STOP
 return U.reg_read(UC_ARM64_REG_X0)
w32(MSG+32,19);assert call('live_ui_system_dispatch',MSG,0)==123
assert boot==1 and r64(STATE+2528)==HEAP
assert struct.unpack('<H',U.mem_read(CORE+128,2))[0]==8
assert r32(CORE+500+6*44)==3177 and r32(CORE+500+7*44)==3158
assert r64(CORE+1296+6*8)==CORE+1480+7*576
# Singleton re-entry never duplicates worker.
call('live_ui_system_dispatch',MSG,0);assert boot==1
meta=struct.unpack('<14I',U.mem_read(S['live_ui_metadata'],56));child=HEAP+meta[8]
assert call('live_ui_menu_page',2,0x705e)==child
assert r32(child)>>16==0x705e
call('live_ui_engine_set',0xa10180000);old=len(calls);assert call('live_ui_engine_get')==0xa10180000;assert len(calls)==old
# Select actual On event callback, preserving native navigation route to Fenestra.
handlers=r64(child+96+24);events=r64(handlers);on=None
for i in range(18):
 event,callback,condition,nxt=struct.unpack('<QQQQ',U.mem_read(events+i*32,32))
 if event&65535==0x286:on=callback;assert condition==0 and nxt==0x7000;break
assert on;call(on,0);assert status==1 and r32(HEAP+meta[4])==1
call('live_ui_poll');assert files['A:\\FENESTRA\\live-z8.1'][24:28]==b'\1\0\0\0';assert call('live_ui_save_error')==0
# Mount event revokes request, then reload chooses saved record.
w32(MSG+32,24);call('live_ui_system_dispatch',MSG,0);assert status==0
call('live_ui_poll');assert status==1
# Dynamic text returns Requested; no readiness fabricated by UI.
call('live_ui_menu_text',1,3177,MSG+100,MSG+108)
label=bytes(U.mem_read(r64(MSG+100),r32(MSG+108))).decode('utf-16le');assert label=='Live Photo: Requested',label
# Exercise both inactive slots repeatedly; exclusive-create would fail at toggle3.
second_events=r64(handlers+8);off=None
for i in range(18):
 event,callback,condition,nxt=struct.unpack('<QQQQ',U.mem_read(second_events+i*32,32))
 if event&65535==0x286:off=callback;break
assert off
generations=[1]
for generation,wanted in [(2,0),(3,1),(4,0),(5,1)]:
 call(on if wanted else off,0);call('live_ui_poll')
 assert call('live_ui_save_error')==0
 assert r64(HEAP+48)==generation
 slot=generation&1;record=files['A:\\FENESTRA\\live-z8.'+str(slot)]
 assert int.from_bytes(record[16:24],'little')==generation
 assert int.from_bytes(record[24:28],'little')==wanted
 w32(MSG+32,24);call('live_ui_system_dispatch',MSG,0);assert status==0
 call('live_ui_poll');assert status==wanted
 generations.append(generation)
# A torn/corrupt newest slot falls back to the other complete generation.
files['A:\\FENESTRA\\live-z8.1'][50]^=1
w32(MSG+32,24);call('live_ui_system_dispatch',MSG,0);call('live_ui_poll')
assert status==0 and r64(HEAP+48)==4
print(json.dumps(dict(pass_=True,boot_calls=boot,menu_rows=8,live_page=hex(0x705e),label=label,
 persisted_paths=list(files),persisted_generations=generations,corrupt_newest_fallback=4,
 file_open_semantics='0x101 exclusive,0x301 truncate',irq_get_native_calls=0,metadata=meta)))
