"""Execute the stock timer branch and rectangle refresh; external rendering is stubbed."""
from pathlib import Path
import sys,json,struct,hashlib
from unicorn import *
from unicorn.arm64_const import *
from layout_data import rows,mapping
b=Path(sys.argv[1]).read_bytes();u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x40160000,(len(b)+4095)&~4095);u.mem_write(0x40160000,b);u.mem_map(0xa02800000,0x900000);u.mem_write(0xa02808000,b[0x4b13000:0x4dc0000]);u.mem_write(0xa02ab5000,b[0x4dc0000:0x4e04950]);u.mem_map(0xa032c0000,0x20000);u.mem_map(0x60000000,0x20000)
def put(a,n,f='<Q'):u.mem_write(a,struct.pack(f,n))
def read(a,f='<Q'):return struct.unpack(f,bytes(u.mem_read(a,struct.calcsize(f))))[0]
D=read(0x42ae79a0+66*8);R=read(D+8);G=0xa032cfc50;STOP=0x60000000;SP=0x60010000
calls=[];rects=[]
stubs={0x4041ef1c,0x403f6544,0x4041eeac,0x4127d850,0x4127d2e0,0x40695e14,0x403f620c,0x403f6204,0x403f657c,0x403f625c,0x403f61ec,0x403f621c,0x403f2920,0x403f65b4,0x403f62b4}
def hook(uc,a,n,_):
 if a==0x4041f150:calls.append(uc.reg_read(UC_ARM64_REG_W2))
 if a==0x403f6204:rects.append(struct.unpack('<4h',bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X1),8))))
 if a in stubs:
  uc.reg_write(UC_ARM64_REG_W0,0);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
u.hook_add(UC_HOOK_CODE,hook);cases=[]
for table,output in ((0,0),(1,1),(2,2),(3,2)):
 put(0xa032d0669,output,'<B')
 for tc in (0,1):
  for elapsed in (0,1):
   put(R+1,0,'<B');put(R+81,tc,'<B');put(R+161,elapsed,'<B')
   put(R+0x90,0x60018000);put(R+0xe0,0x60018100)
   for row in (1,2):
    m=next(m for m in mapping(b) if(m['group'],m['index'])==(66,row))
    for off,t in ((4,min(table,1)),(20,max(table,2))):
     rect=rows(b,t)[m['video'][t]]['P'];u.mem_write(R+row*80+off,struct.pack('<4h',*rect))
   put(G+0x1cc,125,'<I');put(G+0x328,0,'<I');put(G+0x2d1,0,'<B')
   calls.clear();rects.clear();u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_X30,STOP)
   u.emu_start(0x4041daa8,STOP,count=5000);assert u.reg_read(UC_ARM64_REG_PC)==STOP
   want=[1] if tc else [2] if elapsed else[];assert calls==want,(output,tc,elapsed,calls)
   if want:
    rr=struct.unpack('<4h',bytes(u.mem_read(R+want[0]*80+(20 if output==2 else 4),8)));assert rects==[rr],(rects,rr)
   else:assert not rects
   cases.append(dict(table=table,output=output,timecode_enabled=tc,elapsed_enabled=elapsed,updated_rows=list(calls),rectangles=list(rects)))
report={'pass':True,'image_sha256':hashlib.sha256(b).hexdigest(),'cases':cases,'scope':'Actual 4041daa8 branch + 4041f150 cached rectangle selection; renderer/font/format string APIs stubbed. Establishes update priority, not every scene visibility policy.'}
Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+'\n');print('PASS',len(cases),'stock timer branch + rectangle refresh cases')
