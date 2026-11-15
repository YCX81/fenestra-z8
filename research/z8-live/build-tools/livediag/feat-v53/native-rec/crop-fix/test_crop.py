"""Execute exact G crop initialization with/without the proposed bounded hook."""
from pathlib import Path
import hashlib,io,json,struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X19,UC_ARM64_REG_X10,UC_ARM64_REG_X11,UC_ARM64_REG_SP
P=Path(__file__).resolve().parent
main=(P.parent/'v52g.main').read_bytes()
assert hashlib.sha256(main).hexdigest()=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
e=ELFFile(io.BytesIO((P/'crop_fix.o').read_bytes()));assert not e.get_section_by_name('.rela.text')
code=e.get_section_by_name('.text').data();(P/'crop_fix.bin').write_bytes(code)
B=0x40160000;CAVE=0x4019fc00;HOOK=0x40cf7d08
cfg=0xa031957e8;crop=0xa0319b3b8;info=0xa045f08f8;sp=0xa07008000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(main)+4095)&~4095);u.mem_write(B,main)
u.mem_map(0xa03190000,0x20000);u.mem_map(0xa045f0000,0x10000);u.mem_map(0xa07000000,0x10000)
u.mem_write(CAVE,code)
orig=main[HOOK-B:HOOK-B+4]
patch=struct.pack('<I',0x14000000|(((CAVE-HOOK)//4)&0x3ffffff))
def run(codec,size,dim,patched):
 u.mem_write(HOOK,patch if patched else orig)
 # Unicorn caches translated blocks across writes; discard that exact block.
 u.ctl_remove_cache(HOOK,HOOK+4)
 u.mem_write(cfg,struct.pack('<3I',0,codec,size)+bytes(128-12))
 u.mem_write(crop,bytes(64));u.mem_write(info,bytes(12)+struct.pack('<HH',*dim)+bytes(80))
 u.reg_write(UC_ARM64_REG_X19,crop);u.reg_write(UC_ARM64_REG_SP,sp)
 u.emu_start(0x40cf7c88,0x40cf7d14,count=1000)
 return struct.unpack('<4H',u.mem_read(crop+0x14,8))
rows=[]
for codec in range(6):
 for size in range(11):
  for dim in [(1024,682),(1280,852),(1024,684),(1280,854),(3840,2160),(7680,4320),(0,0),(65535,65535)]:
   a=run(codec,size,dim,False);b=run(codec,size,dim,True)
   applies=codec<=1 and size==0 and dim in [(1024,682),(1280,852)]
   assert b==(dim+(0,0) if applies else a),(codec,size,dim,a,b)
   if applies:assert a==(7680,4320,0,0)
   rows.append(dict(codec=codec,size_class=size,file_dimensions=dim,stock_crop=a,patched_crop=b,changed=applies))
r=dict(passed=True,cases=len(rows),changed=sum(x['changed'] for x in rows),code_bytes=len(code),firmware_sha256=hashlib.sha256(main).hexdigest(),scope='Exact stock crop initializer, table and memory instructions executed in Unicorn; prototype hook. Does not validate whole guest or physical scaler.',rows=rows)
(P/'crop-test.json').write_text(json.dumps(r,indent=2)+'\n')
print({k:v for k,v in r.items() if k!='rows'})
