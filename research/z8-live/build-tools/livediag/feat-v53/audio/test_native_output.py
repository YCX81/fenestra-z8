"""Execute unchanged Nikon service+driver; record lower output messages.
The lower DAC/DMA task queue is an observation boundary, not emulated acoustic output.
"""
from pathlib import Path
import struct,json,hashlib
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;FW=P.parents[1]/'feat-v50/integration-writer/v51d/q/main.main';D=FW.read_bytes();B=0x40160000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D)
for a,n in [(0x47000000,0x10000),(0x4e000000,0x40000),(0xa02d4c000,0x1000),(0xa031f8000,0x2000),(0x8a0000000,0x100000)]:u.mem_map(a,n)
END,STACK,MSG,PAY,CALLBACK,QUEUE=0x4e001000,0x4e03f000,0x4e002000,0x4e003000,0x4e004000,0x47000000
w32=lambda a,v:u.mem_write(a,struct.pack('<I',v&0xffffffff));w64=lambda a,v:u.mem_write(a,struct.pack('<Q',v));r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0];r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
# ABI-preserving synchronous queue shim: call original driver dispatcher, return true.
# stp x29,x30,[sp,#-16]!; bl driver; mov w0,1; ldp x29,x30,[sp],16; ret
u.mem_write(QUEUE,struct.pack('<5I',0xa9bf7bfd,0x94000000|(((0x40c81ec0-(QUEUE+4))//4)&0x3ffffff),0x52800020,0xa8c17bfd,0xd65f03c0))
events=[];lower=[]
def ret(v=None):
 if v is not None:u.reg_write(UC_ARM64_REG_X0,v)
 u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(uc,pc,size,data):
 if pc==0x40c80988:u.reg_write(UC_ARM64_REG_PC,QUEUE)
 elif pc in [0x40d2c674,0x40d2c660,0x40d2bcc8,0x40d2bc8c,0x40d2bdf4,0x40d2bd04,0x40c83dcc,0x40c83e3c]:ret(1)
 elif pc in [0x40c7e910,0x40c7dda4]:
  msg=u.reg_read(UC_ARM64_REG_X0);event=r32(msg+32);p=r64(msg+40)
  lower.append(dict(queue=hex(pc),event=hex(event),payload=bytes(u.mem_read(p,40)).hex() if p>=0x40000000 else hex(p)))
  cb=r64(msg+24)
  if cb:u.reg_write(UC_ARM64_REG_W0,event);u.reg_write(UC_ARM64_REG_X1,r64(msg+48));u.reg_write(UC_ARM64_REG_PC,cb)
  else:ret(1)
 elif pc==CALLBACK:events.append(u.reg_read(UC_ARM64_REG_W0));ret()
 elif pc in [0x4127a900,0x4127ab40]:
  a=uc.reg_read(UC_ARM64_REG_X0);b=uc.reg_read(UC_ARM64_REG_X1);n=uc.reg_read(UC_ARM64_REG_W2)
  u.mem_write(a,bytes(u.mem_read(b,n)) if pc==0x4127a900 else bytes([b&255])*n);ret(a)
u.hook_add(UC_HOOK_CODE,hook)
def call(a,args):
 u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,END)
 for reg,val in zip([UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2],args):u.reg_write(reg,val)
 u.emu_start(a,END,count=100000)
 assert u.reg_read(UC_ARM64_REG_PC)==END,hex(u.reg_read(UC_ARM64_REG_PC))
 return u.reg_read(UC_ARM64_REG_W0)
def send(id,payload):
 u.mem_write(MSG,bytes(64));u.mem_write(PAY,bytes(64));u.mem_write(PAY,payload)
 w64(MSG+24,CALLBACK);w32(MSG+32,id);w64(MSG+40,PAY)
 call(0x40d24ae4,[MSG]);return bytes(u.mem_read(PAY,64))
def reset():
 for a,n in [(0xa02d4c000,0x1000),(0xa031f8000,0x2000)]:u.mem_write(a,bytes(n))
 events.clear();lower.clear();w32(0xa02d4cb48,1)
 # Nonzero, deterministic PCM stereo 16-bit pattern.
 u.mem_write(0x8a0000000,struct.pack('<hh',1234,-2345)*240000)

def scenario(config,dirty=0):
 reset()
 # Cache valid, already consumed by previous owner: dirty=0.
 cfg=struct.pack('<7I',0,1,12,0,0,0,0)+bytes(4)
 w32(0xa031f8c4c,1|(dirty<<8));u.mem_write(0xa031f8c50,cfg)
 o=send(0x43180014,struct.pack('<5I',0,5,0,1,0));assert o[20]==1
 after_open=bytes(u.mem_read(0xa02d4cb80,40)).hex()
 if config:
  s=send(0x43180017,cfg);assert s[28]==1
 b=send(0x4318001a,struct.pack('<QIII',0x8a0000000,960000,1,0)+b'\1');assert b[21]==1
 s=send(0x43180015,struct.pack('<IIQII',0,0,CALLBACK,0,0));assert s[20]==1
 return dict(config=config,cache_dirty_before=dirty,cache_header_after=r32(0xa031f8c4c),callbacks=[hex(x) for x in events],after_open_routes=after_open,after_start_routes=bytes(u.mem_read(0xa02d4cb80,40)).hex(),driver_state=r32(0xa02d4cb48),lower=list(lower),source_sha256=hashlib.sha256(bytes(u.mem_read(0x8a0000000,960000))).hexdigest())
res=[scenario(False),scenario(True),scenario(False,1)]
assert res[0]['after_start_routes']==bytes(40).hex()
assert res[1]['after_start_routes']!=res[0]['after_start_routes']
assert res[0]['driver_state']==res[1]['driver_state']==res[2]['driver_state']==3
assert res[2]['after_start_routes']==res[1]['after_start_routes']
assert res[1]['cache_header_after']==res[2]['cache_header_after']==1
out=dict(firmware_sha256=hashlib.sha256(D).hexdigest(),cases=res,boundary='Actual unchanged service/driver instructions; RTOS locks/events/clock and lower task queues shimmed. Nonzero PCM reaches native DMA descriptor, but no physical DAC/speaker or actual DMA drain is proved.')
(P/'native-output.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
