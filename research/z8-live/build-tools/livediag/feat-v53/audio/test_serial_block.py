"""Original serial DMA constructors: prove the 16-bit word count limit."""
from pathlib import Path
import struct,json
from unicorn import *
from unicorn.arm64_const import *
p=Path(__file__).resolve().parent;d=(p.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes();b=0x40160000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(b,(len(d)+4095)&~4095);u.mem_write(b,d)
for a,n in [(0x4e000000,0x10000),(0x0e200000,0x1000)]:u.mem_map(a,n)
end,stk,obj,count=0x4e001000,0x4e00f000,0x4e002000,0x4e003000
records=[]
def r(a,n):return bytes(u.mem_read(a,n))
def hook(u,pc,sz,unused):
 if pc in (0x40c16674,0x40c168c4):
  if pc==0x40c16674:records.append(r(u.reg_read(UC_ARM64_REG_X1),48))
  u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
cases=[]
for pc in (0x40c17954,0x40c17c48,0x40c17cf8):
 for size in (960000,192000,262144):
  records.clear();u.mem_write(obj,bytes(128));u.mem_write(0x0e200108,struct.pack('<I',0x8000));u.reg_write(UC_ARM64_REG_SP,stk);u.reg_write(UC_ARM64_REG_LR,end)
  for reg,v in zip((UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3),(obj,0x170fe400,size if pc==0x40c17954 else 0x0e20013c,count if pc==0x40c17954 else size)):u.reg_write(reg,v)
  u.emu_start(pc,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end and len(records)==1
  words=struct.unpack_from('<H',records[0],10)[0]+1;actual=words*4
  cases.append(dict(pc=hex(pc),requested_bytes=size,descriptor_bytes=actual,descriptor=records[0].hex()))
  assert actual==(173568 if size==960000 else size)
(p/'serial-block-tests.json').write_text(json.dumps(dict(passed=True,cases=cases,boundary='Unchanged original serial constructors execute; lower DMA install/start shim only records descriptor. No hardware output claimed.'),indent=2)+'\n')
print('PASS: all 3 native constructors truncate 960000 -> 173568 bytes; 192000 and 262144 exact')
