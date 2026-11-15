"""Native updater IDs and exact native centered-image arithmetic; no UI claim.
Execute unmodified instructions in delivered G/candidate. Firmware properties,
object creation and backend drawing are explicit fixtures/capture boundaries.
"""
import json,struct,sys,hashlib
from pathlib import Path
from unicorn import *
from unicorn.arm64_const import *
P=Path(__file__).parent;image=Path(sys.argv[1]).read_bytes();B=0x40160000
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(B,(len(image)+4095)&~4095);u.mem_write(B,image)
u.mem_map(0xa02800000,0x900000);u.mem_write(0xa02808000,image[0x4b13000:0x4dc0000]);u.mem_write(0xa02ab5000,image[0x4dc0000:0x4e04950]);u.mem_map(0x60000000,0x30000)
u.mem_map(0xa03200000,0x100000)
SP=0x60010000;STOP=0x60000000;STATE=0xa032cfc50
X=[UC_ARM64_REG_X0+i for i in range(29)]+[UC_ARM64_REG_X29,UC_ARM64_REG_X30]
def put(a,v,fmt='<Q'):u.mem_write(a,struct.pack(fmt,v))
def read(a,fmt):return struct.unpack(fmt,bytes(u.mem_read(a,struct.calcsize(fmt))))
def call(a):
    for r in X:u.reg_write(r,0)
    u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_X30,STOP)
    u.emu_start(a,STOP,count=20000);assert u.reg_read(UC_ARM64_REG_PC)==STOP
captures=[]
def hook(uc,a,n,_):
    if a==0x4041ecb4:
        captures.append([uc.reg_read(X[i]) for i in range(5)])
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
u.hook_add(UC_HOOK_CODE,hook)
# Enable native mapped group rows; the resolver itself is exercised separately.
for g in (34,37,73):
    desc=read(0x42ae79a0+8*g,'<Q')[0];n=read(desc+1,'<B')[0];data=read(desc+8,'<Q')[0]
    for j in range(n):put(data+j*80+1,1,'<B')
identity=[]
for fmt in range(1,7):
    put(STATE+0x130,fmt,'<B');captures.clear();call(0x4041e140)
    assert captures and captures[-1][0]==73
    identity.append(dict(group=73,format=fmt,call=captures[-1]))
assert identity[2]['call'][3:]==[0xa04,0xa62] # H.265 10-bit
put(STATE+0x168,0,'<B');captures.clear();call(0x4041a9ac)
assert captures[-1]==[37,1,0,0x84d,0x9a7]
identity.append(dict(group=37,image_area=0,call=captures[-1]))
# Resolve actual stock mode/format key for the combined 2160/30p glyph. Search
# bounded native enum space, not a substituted ID or fabricated lookup return.
resolution=[]
for codec in range(1,7):
    put(STATE+0x130,codec,'<B')
    for key in range(256):
        put(STATE+0x1a5,key,'<B');captures.clear();call(0x4041a0ac)
        if captures and 0x769 in captures[-1][3:]:
            assert captures[-1][0]==34
            resolution.append(dict(group=34,codec=codec,key=key,call=captures[-1]))
assert resolution
# Exact common non-stretch centering path 4032b1c0..208. Stock PNG dimensions,
# not generic font widths, are inputs. Both opaque plate and white-ink bounds
# stay unmodified; a codec's naturally centered short text is not right-justified.
from layout_data import rows,mapping
maps=mapping(image);centering=[]
for table in (0,1,2,3):
    for group,fg,bg in ((34,0x768,0x769),(37,0x84d,0x9a7),(73,0xa04,0xa62)):
        for j,rid in ((0,bg),(1,fg)):
            m=next(m for m in maps if(m['group'],m['index'])==(group,j))
            row=rows(image,table).get(m['video'][table])
            if row is None:continue
            rect=row['P'];off=0x431d96d0-B+rid*24;n=struct.unpack_from('<I',image,off+4)[0];ptr=struct.unpack_from('<Q',image,off+16)[0]
            blob=image[ptr-B:ptr-B+n];assert blob[:8]==b'\x89PNG\r\n\x1a\n'
            width,height=struct.unpack_from('>II',blob,16)
            dst=0x60018000;u.mem_write(dst,struct.pack('<4h',*rect));u.mem_write(SP+0x98,struct.pack('<2h',width,height));u.mem_write(SP+0x9c,struct.pack('<2h',rect[2]-rect[0],rect[3]-rect[1]))
            u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(X[19],dst);u.reg_write(X[21],0)
            u.emu_start(0x4032b1c0,0x4032b208,count=100)
            x=u.reg_read(X[22])&0xffffffff;y=u.reg_read(X[23])&0xffffffff
            assert(x,y)==(rect[0]+(rect[2]-rect[0]-width)//2,rect[1]+(rect[3]-rect[1]-height)//2)
            centering.append(dict(table=table,group=group,row=j,resource=hex(rid),rect=rect,asset=[width,height],paint=[x,y,x+width,y+height]))
for table,right in ((0,476),(1,476),(2,892),(3,826)):
    for g in (34,37,73):
        painted=[x['paint'] for x in centering if x['table']==table and x['group']==g]
        assert max(p[2] for p in painted)==right,(table,g,painted)
report=dict(pass_=True,sha256=hashlib.sha256(image).hexdigest(),identity=identity,resolution=resolution,centering=centering,
    scope='Actual ARM native format/area/codec updater to drawing-helper boundary, native centered-image arithmetic; input properties/backend explicit fixtures. No full compositor or physical screen validation.')
Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+'\n');print('PASS',len(identity),'identity',len(resolution),'resolution',len(centering),'centering')
