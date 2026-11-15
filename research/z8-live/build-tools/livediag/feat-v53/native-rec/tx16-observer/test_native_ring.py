"""Execute original native ring->serial descriptor chain, no audio task rewrite."""
from pathlib import Path
import struct,json,hashlib
from unicorn import *
from unicorn.arm64_const import *
p=Path(__file__).resolve().parent;d=Path('/path/to/user/fxfix/v53-root/v53f/q/main.main').read_bytes();b=0x40160000
assert hashlib.sha256(d).hexdigest()=='b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(b,(len(d)+4095)&~4095);u.mem_write(b,d)
for a,n in [(0x4e000000,0x10000),(0x0e200000,0x1000),(0xa02d4c000,0x1000)]:u.mem_map(a,n)
end,stk,obj,ring=0x4e001000,0x4e00f000,0x4e002000,0xa02d4c6b8
source=0xa170fe400;physical=0x170fe400;records=[]
r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0]
r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
def w32(a,v):u.mem_write(a,struct.pack('<I',v))
def w64(a,v):u.mem_write(a,struct.pack('<Q',v))
def ret():u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(u,pc,sz,unused):
 if pc==0x40d2bc8c:ret()
 elif pc==0x40cb6bb4:
  src=u.reg_read(UC_ARM64_REG_X2);assert source<=src<source+960000;w32(u.reg_read(UC_ARM64_REG_X3),physical+src-source);ret()
 elif pc in (0x40c16674,0x40c168c4):
  if pc==0x40c16674:
   at=u.reg_read(UC_ARM64_REG_X1);desc=bytes(u.mem_read(at,48));records.append(dict(request=r32(at),fifo=r32(at+44),source=r32(at+40),bytes=(struct.unpack_from('<H',desc,10)[0]+1)*4))
  ret()
u.hook_add(UC_HOOK_CODE,hook)
cases=[]
for block,count in [(960000,1),(192000,5)]:
 for paired,fifo in ((0,0xe200104),(1,0xe200104),(1,0xe20013c)):
  records.clear();u.mem_write(obj,bytes(128));w32(obj+0x30,27);w32(obj+0x34,28);u.mem_write(ring,bytes(64));w32(0xe200108,0x8000);w64(ring,source);w32(ring+8,block);w32(ring+12,count);u.mem_write(ring+20,b'\1');w64(ring+32,obj);w32(ring+40,fifo);u.mem_write(ring+58,bytes([paired]))
  for i in range(10):
   u.reg_write(UC_ARM64_REG_SP,stk);u.reg_write(UC_ARM64_REG_LR,end);u.reg_write(UC_ARM64_REG_X0,ring);u.reg_write(UC_ARM64_REG_W1,(i&1) if paired else 0)
   u.emu_start(0x40c7e2ec,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end
  assert len(records)==10
  for i,r in enumerate(records):assert r["source"]==physical+(i%count)*block and r["bytes"]==(173568 if block==960000 else 192000);assert r["request"] in [27,28] and r["fifo"]==fifo
  cases.append(dict(block_bytes=block,blocks=count,ring_double_bank=paired,fifo=hex(fifo),submissions=list(records)))
# Actual original completion callback refuses refill once its native stop flag is set.
records.clear();u.mem_write(ring+0x39,b'\1')
u.reg_write(UC_ARM64_REG_SP,stk);u.reg_write(UC_ARM64_REG_LR,end);u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_W1,0)
u.emu_start(0x40c7e3d8,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end and not records
stop_pass=True
# A zero byte size is NOT a valid output packet: the native 16-bit count
# underflows to65535, so the private192000B observer must reject it.
records.clear();u.mem_write(ring+0x39,b'\0');w32(ring+0x30,0);w32(ring+8,0);w32(ring+12,1);u.mem_write(ring+0x3a,b'\1');w32(ring+0x28,0xe200104)
u.reg_write(UC_ARM64_REG_SP,stk);u.reg_write(UC_ARM64_REG_LR,end);u.reg_write(UC_ARM64_REG_X0,ring);u.reg_write(UC_ARM64_REG_W1,0)
u.emu_start(0x40c7e2ec,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end and len(records)==1 and records[0]['bytes']==262144
(p/'native-ring-tests.json').write_text(json.dumps(dict(passed=True,firmware_sha256=hashlib.sha256(d).hexdigest(),cases=cases,stop_callback_no_refill=stop_pass,zero_size_constructor=records[0],boundary='Original exact-F ring cursor, serial constructors and stopped callback execute. Runtime request IDs27/28 copied from actual observed native descriptor into a fixture object. Aperture translation, RTOS wake and hardware DMA install/start leaves are recorded shims. No speaker/DAC assertion.'),indent=2)+'\n')
print('PASS 60 native constructor calls, stopped callback refuses refill, zero-byte input produces rejected262144B descriptor')
