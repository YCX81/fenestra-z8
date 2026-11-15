"""Execute exact stock table lookup, EVF size selector and rectangle copy in Unicorn.
No renderer/display MMIO is simulated. Both before/after records and orientation
indices are checked; the native gate/getter gets left/right inputs separately.
"""
import json,struct,sys,hashlib
from pathlib import Path
from unicorn import *
from unicorn.arm64_const import *
ROOT=Path(__file__).parent
image=Path(sys.argv[1]).read_bytes();manifest=json.loads(Path(sys.argv[2]).read_text());BASE=0x40160000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(BASE,(len(image)+4095)&~4095);u.mem_write(BASE,image)
u.mem_map(0xa02800000,0x900000);u.mem_write(0xa02808000,image[0x4b13000:0x4dc0000]);u.mem_write(0xa02ab5000,image[0x4dc0000:0x4e04950]);u.mem_map(0x60000000,0x20000)
SP=0x60010000
X=[UC_ARM64_REG_X0+i for i in range(29)]+[UC_ARM64_REG_X29,UC_ARM64_REG_X30]
def put(a,v,fmt='<Q'):u.mem_write(a,struct.pack(fmt,v))
def read(a,fmt):return struct.unpack(fmt,bytes(u.mem_read(a,struct.calcsize(fmt))))
count=0;records=manifest['edits'];examples=[]
for after in (False,True):
 for e in records:u.mem_write(int(e['address'],16),bytes.fromhex(e['replacement' if after else 'expected']))
 for e in records:
  i=e['table'];size=max(0,i-2);g=e['group'];row=e['index'];table=0x42ae6c20
  desc=read(0x42ae79a0+g*8,'<Q')[0];data=read(desc+8,'<Q')[0]
  for orientation in (0,1):
   u.reg_write(UC_ARM64_REG_SP,SP);put(SP+0x8c,size,'<I');put(SP+0x80,orientation)
   for reg in X:u.reg_write(reg,0)
   if i==0:
    # Execute real group-ID matching and initial LCD rectangle cache copy.
    # Upstream object creation is mocked; source table and row map are native.
    start,end=0x4041158c,0x40411608
    put(SP+0x31c,e['id'],'<I')
    regs={0:read(desc+1,'<B')[0],19:0x60018000,20:g,21:0x43a21a70,22:(int(e['address'],16)-0x43a21a70)//68,26:orientation,27:48,28:68,29:0x42ae79a0};cache=4
   else:
    start=0x40411864;end=0x404118e0 if i==1 else 0x404118f0
    regs={8:1 if i==1 else 2,11:size,19:0,20:80,21:1,22:0,24:0 if i==1 else 8,25:table,26:3,27:row*80+12,28:row,29:desc};cache=12 if i==1 else 20
   for k,v in regs.items():u.reg_write(X[k],v)
   u.reg_write(UC_ARM64_REG_SP,SP)
   u.emu_start(start,end,count=10000)
   assert u.reg_read(UC_ARM64_REG_PC)==end
   got=read(data+row*80+cache,'<4h');record=bytes.fromhex(e['replacement' if after else 'expected']);want=struct.unpack_from('<4h',record,24+orientation*8)
   assert got==want,(after,i,g,row,got,want);count+=1
   if after and e['id'] in (0x2002,0x27df,0x2164,0x2848):examples.append({'table':i,'orientation':orientation,'id':hex(e['id']),'rect':got})
# Actual orientation getter plus original gated policy. Set property getter to a
# stub returning configured auto-rotate; execution of gate/policy/getter is real.
GATE=0xa116d3480;u.mem_map(GATE&~4095,4096);u.mem_map(0xa03228000,4096)
STOP=0x60000000;put(STOP,0xd65f03c0,'<I')
auto=1
def hook(uc,a,n,_):
 if a==0x403ec398:
  uc.reg_write(UC_ARM64_REG_W0,auto);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
u.hook_add(UC_HOOK_CODE,hook)
poses=[]
for gate in (0,1):
 for auto_value in (0,1):
  auto=auto_value
  for pose in (0,1,2,3):
   put(GATE,gate,'<I');put(0xa03228308,0,'<I');put(0xa03228244,pose,'<I')
   u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_X30,STOP);u.reg_write(UC_ARM64_REG_W0,1);u.reg_write(UC_ARM64_REG_W1,1)
   u.emu_start(0x4038e3e4,STOP,count=300)
   u.reg_write(UC_ARM64_REG_X30,STOP);u.reg_write(UC_ARM64_REG_SP,SP)
   u.emu_start(0x4038d658,STOP,count=300)
   got=u.reg_read(UC_ARM64_REG_W0);want=int(gate and auto and pose in(1,2));assert got==want,(gate,auto,pose,got,want)
   poses.append({'gate':gate,'auto_rotate':auto,'pose':pose,'portrait_index':got})
report={'pass':True,'image_sha256':hashlib.sha256(image).hexdigest(),'record_cases':count,'unique_records':len(records),'orientation_policy_cases':len(poses),'examples':examples,'poses':poses,'scope':'Exact ARM native LCD ID-match/cache copy, secondary/EVF table resolver/size selector/copy and gate/getter; excludes full UI rendering and display hardware'}
Path(sys.argv[3]).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in('examples','poses')}))
