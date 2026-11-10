"""Execute v2 linked ticket hooks with external native functions stubbed."""
import importlib.util,subprocess,struct,json
from pathlib import Path
from unicorn import *
import unicorn.arm64_const as A
spec=importlib.util.spec_from_file_location('hooks',Path(__file__).with_name('hook_test.py'));hooks=importlib.util.module_from_spec(spec);spec.loader.exec_module(hooks)
elf=Path('/path/to/user/.local/share/nikon-z8-qemu/fenestra-z8/bundle/live-a/v4/payload.elf');blob=elf.read_bytes();nm=subprocess.check_output(['llvm-nm','--defined-only',str(elf)],text=True);sym={p[2]:int(p[0],16) for l in nm.splitlines() if len(p:=l.split())==3}
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
def mp(a,n):
 for p in range(a&~4095,(a+n+4095)&~4095,4096):
  if p not in pages:u.mem_map(p,4096);pages.add(p)
for a,n,b in hooks.segments(blob):mp(a,n);u.mem_write(a,b)
for a in [0x4085b320,0x4085b6d4,0x406bd954,sym['memset'],sym['memcpy'],0x70000000,0x71000000,0x72000000,0x73000000,0x7fff0000]:mp(a,0x10000 if a==0x70000000 else 4096)
state=0x71000000;obj=0x72000000;payload=0x73000000;stop=0x7fff0000
u.mem_write(state,struct.pack('<II',0x4c415231,12512));u.mem_write(obj+12,struct.pack('<I',3));u.mem_write(obj+0x28,struct.pack('<I',42));p=b'A:/DCIM/100NCZ_8/DSC_0001.JPG';u.mem_write(obj+0x74,p+b'\0');u.mem_write(obj+0x94,struct.pack('<I',len(p)));u.mem_write(payload,struct.pack('<I',42))
regs=[getattr(A,'UC_ARM64_REG_X'+str(i)) for i in range(31)]
def trace(cpu,pc,size,data):
 if pc in (sym['live_ui_engine_get'],0x4085b320,0x4085b6d4,0x406bd954,sym['memset'],sym['memcpy']):
  if pc==sym['live_ui_engine_get']:ret=state
  elif pc in (0x4085b320,0x4085b6d4):ret=0x04320080
  elif pc==0x406bd954:ret=0
  elif pc==sym['memset']:
   dst,val,n=(cpu.reg_read(regs[i]) for i in range(3));cpu.mem_write(dst,bytes([val&255])*n);ret=dst
  else:
   dst,src,n=(cpu.reg_read(regs[i]) for i in range(3));cpu.mem_write(dst,bytes(cpu.mem_read(src,n)));ret=dst
  cpu.reg_write(regs[0],ret);cpu.reg_write(A.UC_ARM64_REG_PC,cpu.reg_read(regs[30]))
 elif pc==stop:cpu.emu_stop()
u.hook_add(UC_HOOK_CODE,trace)
def call(name,*args):
 for i,x in enumerate(args):u.reg_write(regs[i],x)
 u.reg_write(A.UC_ARM64_REG_SP,0x7000fff0);u.reg_write(regs[30],stop);u.emu_start(sym[name],stop,count=100000);assert u.reg_read(A.UC_ARM64_REG_PC)==stop
get32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0]
get64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
t=state+1976;descriptor=0xa049d55b0+3*0x21c8
call('live_open_wrap',obj);assert get64(t)==descriptor and get32(t+32)==3 and get32(t+36)==42 and get32(t+44)==1
call('live_finalize_wrap',obj,123456);assert get64(t+16)==123456
call('live_closed_notify',descriptor);assert get32(t+52)==1 and get32(t+48)==0
call('live_event_wrap',0x04180002,payload);assert get32(t+48)==1
old=get64(t+8);call('live_open_wrap',obj);assert get64(t+8)>old and get32(t+48)==0 and get32(t+52)==0
print(json.dumps({'passed':True,'checks':['open fd/id/path and descriptor','final size','clear before success','matching event','fd reuse clears old generation'],'limitations':'native filesystem, concurrency, time and card switching stubbed'},indent=2))
