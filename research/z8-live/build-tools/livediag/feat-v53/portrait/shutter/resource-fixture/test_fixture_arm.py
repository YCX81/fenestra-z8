from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;B=0x40160000;RES=0xa15aab8b8
blob=(P/'resource4e-tone2-synthetic.bin').read_bytes();manifest=json.loads((P/'manifest.json').read_text())
assert hashlib.sha256(blob).hexdigest()==manifest['loader']['sha256']
images={'G':P.parent.parent/'v52g.main','F':P.parent/'production/review/v53f.main'};rows=[]
for name,path in images.items():
 d=path.read_bytes();u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(d)+4095)&~4095);u.mem_write(B,d)
 for a,n in [(0x4e000000,0x10000),(0xa02910000,0x10000),(0xa038fe000,0x2000),(RES&~4095,0x11000)]:u.mem_map(a,n)
 desc=0xa0291c948+0x4e*48;bank=0xa0291dab8+24*40
 for addr,n in [(desc,48),(bank,40)]:
  off=addr-0xa02808000+0x4b13000;u.mem_write(addr,d[off:off+n])
 u.mem_write(desc+16,struct.pack('<Q',RES));u.mem_write(desc+24,struct.pack('<I',len(blob)));u.mem_write(RES,blob)
 def call(pc,arg=0,end=0x4e001000):
  u.reg_write(UC_ARM64_REG_SP,0x4e00f000);u.reg_write(UC_ARM64_REG_LR,end);u.reg_write(UC_ARM64_REG_X0,arg)
  u.emu_start(pc,end,count=10000);assert u.reg_read(UC_ARM64_REG_PC)==end
  return u.reg_read(UC_ARM64_REG_X0)
 call(0x40e34184,end=0x40e342e0)
 ptr=call(0x40e3469c,2);size=call(0x40e348f4,2);duration=call(0x40e349e8,2)
 pcm=bytes(u.mem_read(ptr,size));assert ptr==RES+64 and size==4800
 assert hashlib.sha256(pcm).hexdigest()==manifest['pcm']['sha256']
 rows.append(dict(image=name,firmware_sha256=hashlib.sha256(d).hexdigest(),resource_base=hex(RES),tone2_pointer=hex(ptr),tone2_bytes=size,native_duration_ms=duration,pcm_sha256=hashlib.sha256(pcm).hexdigest()))
(P/'fixture-arm.json').write_text(json.dumps(dict(passed=True,cases=rows,boundary='Native resource lookup/init, data/length/duration getters execute; preloaded raw resource RAM and descriptor pointer are input fixtures, no DMA or real timing claimed.'),indent=2)+'\n');print(rows)
