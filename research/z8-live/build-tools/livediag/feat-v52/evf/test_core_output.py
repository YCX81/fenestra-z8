#!/usr/bin/env python3
"""Run the pinned core attach/is_current instructions with the real new ARM helpers.
Native GUI endpoints are ABI mocks; this is not a physical panel test."""
from pathlib import Path
import struct,subprocess,json
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
import core_output_patch
D=Path(__file__).resolve().parent;BASE=0x40160000
sy={p[2]:int(p[0],16) for l in subprocess.check_output(['llvm-nm','-n',str(D/'pb/payload.elf')],text=True).splitlines() if len(p:=l.split())==3}
base=Path('/path/to/user/fxfix/v51d/q/main.main').read_bytes();b,changes=core_output_patch.apply(base,sy)
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(BASE,(len(b)+4095)&~4095);u.mem_write(BASE,bytes(b));u.mem_write(0x40196000,(D/'pb/payload.bin').read_bytes())
u.mem_map(0xa02000000,0x2000000);u.mem_map(0xa10000000,0x100000);u.mem_map(0xa20000000,0x100000)
ROOT=0xa03229ee0+4*0x118;VIEW=0xa0284a0e0;DISPLAY=0xa10000000;SCENE=DISPLAY+0x20000;STACK=0xa200ff000;END=0x40160000
selected=1;children=0;calls=[];checks=0
regs=[UC_ARM64_REG_X0+i for i in range(4)]
def w(a,v,n=4):u.mem_write(a,int(v).to_bytes(n,'little'))
def rd(a,n=4):return int.from_bytes(u.mem_read(a,n),'little')
def ret(v=0):u.reg_write(UC_ARM64_REG_X0,v);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_X30))
def native(uc,a,size,user):
 global children
 if a==END:uc.emu_stop();return
 x=[uc.reg_read(r) for r in regs]
 if a in [0x40d2bfb4,0x40d2bf60,0x4031e36c,0x4031e3a8,0x403f1790]:ret();return
 if a==0x403dbdb4:ret();return
 if a==0x40393f6c:ret(3);return # core shooting owner
 if a==0x40393f28:ret(1);return
 if a==0x4039a0b4:assert x[1]==4;ret(ROOT);return
 if a==0x403f1b18:ret(1);return
 if a==0x40398570:ret(selected);return
 if a==0x403f0db8:assert x[0]==4;ret(4);return
 if a==0x40340d64:
  assert x[:3]==[selected,1,4],x
  calls.append(['backing',*x[:3]])
  uc.mem_write(x[3],struct.pack('<4H',0,0,1280 if selected==1 else 1920,720 if selected==1 else 1080));ret(1);return
 if a==0x40c7a304:assert x[0]==ROOT+0xc8;ret(children);return
 if a==0x40325e0c:
  assert x[0]==ROOT and children==0
  v=bytes(uc.mem_read(x[1],8));uc.mem_write(ROOT+0x10,v);uc.mem_write(ROOT+0x60,v);ret();return
 if a==0x403297e4:
  assert x[0]==ROOT and children==0
  uc.mem_write(ROOT,bytes(uc.mem_read(x[1],0x18)));uc.mem_write(ROOT+0x108,bytes(uc.mem_read(x[1]+0x70,12)))
  w(ROOT+0x48,0x40325a80,8);w(ROOT+0xb0,0x4032987c,8);ret();return
 if a==0x403f0b58:ret();return
 if a==0x40c7a6b4:ret();return
 if a==0x4031e3d8:ret((1280 if selected==1 else 1920)|((720 if selected==1 else 1080)<<16));return
 if a==0x403f0b54:assert x[:2]==[ROOT,DISPLAY];children+=1;ret(1);return
 if a==0x403f179c:ret();return
 if a in [0x403409d4,0x403409a4]:
  assert x[:3]==[selected,1,4],x;calls.append(['enable' if a==0x403409a4 else 'enabled',*x[:3]]);ret();return
 if 0x40200000<=a<0x41200000:raise AssertionError(('unmocked native',hex(a),[hex(v) for v in x]))
u.hook_add(UC_HOOK_CODE,native)
w(0xa032280c8,VIEW,8)
def call(fn,args):
 u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_X30,END)
 for i,v in enumerate(args):u.reg_write(regs[i],v)
 u.emu_start(fn,END,count=200000);return u.reg_read(UC_ARM64_REG_X0)
def fresh():
 global children
 children=0;u.mem_write(DISPLAY,bytes(0x2000))
# Initial LCD, then stale output detection, then EVF and return LCD. The prior
# child is removed by the native owner before the next attachment, as required.
for dev in [1,4,1]:
 selected=dev;fresh();assert call(0x401a3c70,[DISPLAY,SCENE])==0
 assert rd(DISPLAY+0xe0)==1 and rd(ROOT+0x10c)==dev
 assert call(0x401a296c,[DISPLAY])==1;checks+=1
 selected=4 if dev==1 else 1
 assert call(0x401a296c,[DISPLAY])==0;checks+=1
# Explicitly refuse to repurpose the still-linked root.
assert children==1
assert call(sy['pb_output_initialize_global'],[ROOT])==5 # FX_BUSY
checks+=1
# Foreign identity preserved by initializer; core validator rejects stale output.
w(ROOT,0x1234);before=bytes(u.mem_read(ROOT,0x118));fresh()
assert call(0x401a3c70,[DISPLAY,SCENE])!=0
assert bytes(u.mem_read(ROOT,0x118))==before;checks+=1
result=dict(actual_pinned_core_execution=True,arm_helpers=True,checks=checks,passed=True,core_patch_bytes=40,changes=changes,native_abi_calls=calls,physical_panel_test=False)
(D/'core-output-tests.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ['changes','native_abi_calls']}))
