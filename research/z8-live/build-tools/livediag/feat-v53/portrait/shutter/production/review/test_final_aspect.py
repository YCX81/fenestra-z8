"""Independent exact-G ARM review of root's 64-byte native aspect wrapper.
No firmware or root source file is modified. Portrait inputs are explicit helper
fixtures, not claims about the whole orientation/EVF routing pipeline.
"""
from pathlib import Path
import hashlib,json,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
R=Path(__file__).resolve().parent;B=0x40160000;CAVE=0x4019ffc0;ENTRY=0x407138d0
fw=(R/'v53f.main').read_bytes();blob=fw[CAVE-B:CAVE-B+64];assert blob==(R/'aspect_fix.bin').read_bytes()
assert hashlib.sha256(fw).hexdigest()=='b0cd7c40c3c6ffdf07b87fcbab20b98fbe9ed0c2a848550855d989b9e64fd8b0'
assert len(blob)==64
word=struct.unpack_from('<I',fw,ENTRY-B)[0];assert word>>26==5 and ENTRY+(((word&0x3ffffff)^0x2000000)-0x2000000)*4==CAVE
ORIGINAL=struct.pack('<I',0x14000000|(((0x409708f0-ENTRY)//4)&0x3ffffff))
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(fw)+4095)&~4095);u.mem_write(B,fw);u.mem_write(CAVE,blob)
for a,n in [(0xa03d50000,0x10000),(0xa045f0000,0x10000),(0xa0ff6f000,0x1000),(0xa10000000,0x100000)]:u.mem_map(a,n)
P=0xa10001000;Q=P+0x100;SP=0xa100f0000;END=B;ANAM=0xa03d5c0ad;CORE=0xa0ff6f000;INFO=0xa045f07d8
X=[globals()['UC_ARM64_REG_X'+str(i)] for i in range(29)]+[UC_ARM64_REG_FP,UC_ARM64_REG_LR]
traces=[];writes=[]
def hook(uc,pc,size,data):
 if pc in [0x409708f0,0x40960a00,0x407138d4]:traces.append((pc,[uc.reg_read(X[i]) for i in range(5)]))
def mem(uc,access,addr,size,value,data):
 assert addr>=0x40000000,('unexpected MMIO',hex(addr))
 if access==17:writes.append((addr,size))
u.hook_add(UC_HOOK_CODE,hook);u.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,mem)
def desc(w,h,active=1,x=0,y=0,par=(1,1)):
 b=bytearray(28);struct.pack_into('<14H',b,0,active,((w+63)//64)*64,h,x,y,x+w,y+h,*par,0,2,0,0,0);return bytes(b)
def run(dim,mode,record,anam,patched,force=False):
 global traces,writes
 u.mem_write(ENTRY,struct.pack('<I',0x14000000|(((CAVE-ENTRY)//4)&0x3ffffff)) if patched else ORIGINAL);u.ctl_remove_cache(ENTRY,ENTRY+4)
 u.mem_write(P,record);u.mem_write(Q-16,bytes([0xa5])*60);u.mem_write(ANAM,bytes([anam]));u.mem_write(CORE,bytes(0x20));u.mem_write(INFO,bytes(0x40))
 if force:u.mem_write(CORE+8,struct.pack('<Q',(1<<32)|0x1234));u.mem_write(INFO,struct.pack('<I',0x1234));u.mem_write(INFO+0x14,b'\x01')
 sent={i:0x1234500000000000+i for i in range(19,30)}
 for i,v in sent.items():u.reg_write(X[i],v)
 for i,v in enumerate([*dim,mode,P,Q]):u.reg_write(X[i],v)
 u.reg_write(UC_ARM64_REG_LR,END);u.reg_write(UC_ARM64_REG_SP,SP);traces=[];writes=[]
 u.emu_start(ENTRY,END,count=50000);assert u.reg_read(UC_ARM64_REG_PC)==END
 assert u.reg_read(UC_ARM64_REG_SP)==SP
 assert all(u.reg_read(X[i])==v for i,v in sent.items())
 assert bytes(u.mem_read(P,28))==record and bytes(u.mem_read(Q-16,16))==bytes([0xa5])*16 and bytes(u.mem_read(Q+28,16))==bytes([0xa5])*16
 assert len(traces)==3 and traces[0][0]==0x409708f0 and traces[1][0]==0x40960a00 and traces[2][0]==0x407138d4
 for addr,n in writes:assert Q<=addr and addr+n<=Q+28 or SP-0x1000<=addr and addr+n<=SP
 return bytes(u.mem_read(Q,28)),traces
actual=bytes.fromhex(json.loads((R/'actual-input.json').read_text())['raw_4_records_hex'])
inputs={f'actual-caller-record-{i}':actual[i*28:(i+1)*28] for i in range(4)}
inputs.update({'LCD-portrait-fixture':desc(682,1024),'EVF-landscape-fixture':desc(1280,960),'EVF-portrait-fixture':desc(960,1280),'HDMI-16x9-fixture':desc(1920,1080),'offset-letterbox-fixture':desc(1024,682,x=32,y=48),'PAR-2x1-fixture':desc(1024,682,par=(2,1)),'PAR-1x2-fixture':desc(1024,682,par=(1,2))})
dims=[(1024,682),(1280,852),(1024,680),(1024,684),(1280,854),(682,1024),(852,1280),(1920,1080),(3840,2160),(7680,4320),(0,0),(65535,65535)]
rows=[];changed=0
for dim in dims:
 for mode in [0,1,2,0xffffffff]:
  for name,record in inputs.items():
   for anam in range(8):
    for force in [False,True]:
     base,bt=run(dim,mode,record,anam,False,force);out,tr=run(dim,mode,record,anam,True,force)
     applies=mode==0 and dim in [(1024,682),(1280,852)]
     effective=1 if applies else mode
     # The changed call must exactly equal original NFP + stock mode1, all 28 bytes.
     ref,rt=run(dim,effective,record,anam,False,force) if applies else (base,bt)
     assert out==ref,(dim,mode,name,anam,force,base.hex(),out.hex(),ref.hex())
     assert tr[0][1]==[*dim,effective,P,Q]
     assert tr[1][1]==[*dim,effective,P,Q]
     if not applies:assert tr==bt
     changed+=out!=base
     rows.append(dict(dim=dim,mode=mode,input=name,anamorphic=anam,nfp_photo_override=force,guard_matches=applies,changed=out!=base,output_hex=out.hex()))
# Quantify representative viewports, including original anamorphic reduction.
def unpack(raw):
 v=struct.unpack('<14H',raw);return dict(active=v[0]&255,pitch=v[1],buffer_height=v[2],rect=list(v[3:7]),width=v[5]-v[3],height=v[6]-v[4],par=list(v[7:9]))
examples=[]
for dim in [(1024,682),(1280,852),(3840,2160)]:
 for name in ['actual-caller-record-0','EVF-landscape-fixture','LCD-portrait-fixture','EVF-portrait-fixture','offset-letterbox-fixture']:
  record=inputs[name]
  for anam in range(8):
   a,_=run(dim,0,record,anam,False);b,_=run(dim,0,record,anam,True)
   examples.append(dict(source=dim,input=name,anamorphic=anam,before=unpack(a),after=unpack(b)))
# Strict crop/centering tests apply to square-pixel positive fixture rectangles.
for x in examples:
 a=x['after'];inp=unpack(inputs[x['input']]);l,t,r,b=a['rect'];il,it,ir,ib=inp['rect']
 assert il<=l<r<=ir and it<=t<b<=ib,(x,'out of input bounds')
 assert abs((l+r)-(il+ir))<=2 and abs((t+b)-(it+ib))<=4,(x,'not centered')
 # Native scalar enforces 16/8 alignment; NFP reduces height to another multiple of8.
 assert a['height']%8==0 and a['width']%16==0
res=dict(passed=True,cases=len(rows),changed_outputs=changed,main_sha256=hashlib.sha256(fw).hexdigest(),wrapper_sha256=hashlib.sha256(blob).hexdigest(),wrapper_bytes=len(blob),hook=hex(ENTRY),cave=hex(CAVE),original_entry_target='0x409708f0',nfp_ratios=['off','5/4','13/10','4/3','3/2','8/5','9/5','2/1'],scope='Exact final F NFP wrapper, trampoline, stock helper, memcpy and ratio table execute. 4 caller records are captured; portrait/EVF active/offset/PAR variants are labelled fixtures. No display MMIO. No whole orientation pipeline or physical display proof.',examples=examples)
(R/'final-aspect-result.json').write_text(json.dumps(res,indent=2)+'\n');(R/'final-aspect-matrix.json').write_text(json.dumps(rows,separators=(',',':'))+'\n')
print({k:v for k,v in res.items() if k!='examples'})
for x in examples:
 if x['source']==(1024,682) and x['anamorphic']==0:print(x)
