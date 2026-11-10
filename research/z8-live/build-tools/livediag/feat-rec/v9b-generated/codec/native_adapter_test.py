"""Execute adapter AArch64 only; native entrypoints are ABI-checking stubs.
This is NOT a native filesystem/collision test."""
import json,pathlib,struct,subprocess,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
out=pathlib.Path(sys.argv[1]);obj=out/'native_files.aarch64.o';elf=out/'native-adapter-test.elf';raw=out/'native-adapter-test.bin'
subprocess.run(['ld.lld','-Ttext=0x50000000','-e','live_native_file_ops',str(obj),'-o',str(elf)],check=True)
subprocess.run(['llvm-objcopy','-O','binary',str(elf),str(raw)],check=True)
symbols={line.split()[2]:int(line.split()[0],16) for line in subprocess.check_output(['llvm-nm',str(elf)],text=True).splitlines() if len(line.split())==3}
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x50000000,0x10000);u.mem_write(0x50000000,raw.read_bytes());u.mem_map(0x402dd000,0x1000);u.mem_map(0x60000000,0x20000)
STOP=0x5000f000;CTX=0x60000000;OPS=CTX+0x100;PATH=CTX+0x200;COUNT=CTX+0x300;BUF=CTX+0x400
u.mem_write(PATH,b'A:\\DCIM\\100NZ_8\\LIVE0001.MOV\0')
state={'events':[],'error':False,'seek_fail':False,'open_result':7}
regs=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4]
def hook(uc,a,size,data):
 if a==STOP:uc.emu_stop();return
 if 0x402dd000<=a<0x402de000:
  x=[uc.reg_read(r) for r in regs];state['events'].append([hex(a),x]);ret=0
  if a==0x402dd4f8:assert x[0]==PATH and x[1] in (0,0x101) and x[2]==0x20;ret=state['open_result']
  elif a==0x402dd8e8:assert x[0]==7 and x[1]==123 and x[2]==0 and x[3]==COUNT;uc.mem_write(x[3],struct.pack('<Q',123));ret=not state['seek_fail']
  elif a in (0x402dd6c8,0x402dd78c):
   assert x[0]==7 and x[1]==BUF and x[2]==3
   if a==0x402dd78c:assert x[4]==0
   uc.mem_write(x[3],struct.pack('<I',3));ret=0x80000001 if state['error'] else 0
  elif a==0x402dd558:assert x[0]==7;ret=0x80000001 if state['error'] else 0
  elif a==0x402dda4c:assert x[0]==PATH;ret=0x80000001 if state['error'] else 0
  elif a in (0x402ddf00,0x402ddf04):assert x[0]==0x12345678;ret=1 if a==0x402ddf00 else 0
  else:raise AssertionError(hex(a))
  uc.reg_write(UC_ARM64_REG_X0,int(ret)&0xffffffff);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(address,*args):
 for r,v in zip(regs,args):u.reg_write(r,v)
 u.reg_write(UC_ARM64_REG_SP,0x6001f000);u.reg_write(UC_ARM64_REG_LR,STOP);u.emu_start(address,STOP,count=10000)
 return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
call(symbols['live_native_file_ops'],OPS,CTX)
ops=struct.unpack('<10Q',u.mem_read(OPS,80));assert ops[0]==CTX
for i in (1,2):assert call(ops[i],CTX,PATH,COUNT)==0 and struct.unpack('<I',u.mem_read(COUNT,4))[0]==7
state['open_result']=0x80000002;assert call(ops[2],CTX,PATH,COUNT)==0xffffffff
state['open_result']=7
assert call(ops[3],CTX,7,123,COUNT)==0
state['seek_fail']=True;assert call(ops[3],CTX,7,123,COUNT)==0xffffffff;state['seek_fail']=False
for i in (4,5):
 assert call(ops[i],CTX,7,BUF,3,COUNT)==0 and struct.unpack('<Q',u.mem_read(COUNT,8))[0]==3
 state['error']=True;assert call(ops[i],CTX,7,BUF,3,COUNT)==0xffffffff;state['error']=False
for i,arg in ((6,7),(7,PATH)):
 assert call(ops[i],CTX,arg)==0;state['error']=True;assert call(ops[i],CTX,arg)==0xffffffff;state['error']=False
before=len(state['events']);assert call(ops[3],CTX,7,1<<63,COUNT)==0xffffffff;assert call(ops[4],CTX,7,BUF,1<<32,COUNT)==0xffffffff;assert len(state['events'])==before
assert call(ops[8],CTX)==0;call(ops[9],CTX)
u.mem_write(CTX,struct.pack('<3Q',0x12345678,0x402ddf00,0x402ddf04));assert call(ops[8],CTX)==1;call(ops[9],CTX)
assert struct.unpack('<I',u.mem_read(CTX+32,4))[0]==0xffffffff
print(json.dumps({'result':'PASS','native_calls_stubbed':len(state['events']),'checks':['read/create flags and attr','exclusive existing error','write fifth argument NULL','32-bit done widened to size_t','seek success inversion','I/O failures propagated','seek/transfer bounds rejected without native call','null and caller-context cancellation/yield hooks'],'limitations':'Native functions stubbed; no QEMU, native filesystem, concurrency or collision/hash observation'},indent=2))
