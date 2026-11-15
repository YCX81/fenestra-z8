"""Execute original target-to-compositor transform with captured descriptors.
No display registers, guest execution state, or firmware files are modified.
"""
from pathlib import Path
import struct,json,hashlib
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *
R=Path('/path/to/user/fxfix/v53-evf');FW=Path('/path/to/user/fxfix/v52-root/v52g/q/main.main')
b=FW.read_bytes();B=0x40160000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(b)+4095)&~4095);u.mem_write(B,b)
u.mem_map(0xa02800000,0x800000);u.mem_map(0xa10000000,0x100000)
END=B;STACK=0xa100ff000;P=0xa10001000
panel_mode=0xc5

def hook(uc,pc,size,data):
 if pc==0x40330a6c:
  uc.reg_write(UC_ARM64_REG_X0,panel_mode);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,*args):
 u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,END)
 for reg,v in zip([UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3],args):u.reg_write(reg,v)
 u.emu_start(pc,END,count=50000);assert u.reg_read(UC_ARM64_REG_PC)==END
 return u.reg_read(UC_ARM64_REG_X0)
rows=[]
for out,composite in [(1,(1024,682)),(4,(1280,960))]:
 fn=R/'run-eye8-g'/f'output-{out}-014.18.bin';blk=fn.read_bytes()
 u.mem_write(0xa02f35f98+(0 if out==1 else 2)*0x1938,blk)
 for target in (2,4):
  for panel_mode in (0xc5,1):
   u.mem_write(P,bytes(0x80))
   size=call(0x4034079c,out,1,target);u.mem_write(P,struct.pack('<I',size))
   call(0x40340b9c,out,1,target,P+4)
   call(0x40340d64,out,1,target,P+8)
   call(0x40340c78,out,1,target,P+16)
   mag=call(0x403407cc,out,1,target);u.mem_write(P+24,struct.pack('<H',mag))
   rot=call(0x4033fa5c,out,1,target);u.mem_write(P+28,struct.pack('<I',rot))
   u.mem_write(P+32,struct.pack('<HH',*composite))
   before=bytes(u.mem_read(P,36));call(0x40332820,out,1,P)
   raw=struct.unpack('<4i',bytes(u.mem_read(P+36,16)));rect=[v/65536 for v in raw]
   dims=struct.unpack_from('<HH',before)
   bbox=None
   if target==4:
    bx0,by0,bx1,by1=1,101,108,139
    bbox=[rect[0]+bx0*(rect[2]-rect[0])/dims[0],rect[1]+by0*(rect[3]-rect[1])/dims[1],rect[0]+bx1*(rect[2]-rect[0])/dims[0],rect[1]+by1*(rect[3]-rect[1])/dims[1]]
    assert bbox[2]>0 and bbox[0]<composite[0] and bbox[3]>0 and bbox[1]<composite[1]
   rows.append(dict(output=out,target=target,panel_mode_fixture=hex(panel_mode),source=list(dims),composite_size=list(composite),params_hex=before.hex(),destination_fixed=list(raw),destination=rect,live_ink_bbox=bbox))
result=dict(passed=True,main_sha256=hashlib.sha256(b).hexdigest(),scope='Original ARM getters and 40332820 transform on actual captured descriptor blocks. Composite sizes correspond to observed native graphics job extents; panel mode c5 and alternate are fixtures. This is not physical EVF visibility proof.',cases=rows)
(R/'native-geometry-arm.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
