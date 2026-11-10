from pathlib import Path
import hashlib,struct,json,io,zlib
from PIL import Image
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import *
B=0x40160000;D=Path('bg34.main').read_bytes();u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
u.mem_map(B,(len(D)+4095)&~4095);u.mem_write(B,D)
u.mem_map(0xa02800000,0x1000000);u.mem_write(0xa02808000,D[0x4b13000:0x4dc0000])
u.mem_map(0xa1162b000,0x100000)
spans=json.loads(Path('prototype/boot-spans.json').read_text())
for x,n,a in spans:u.mem_write(0xa1162b000+x,D[a-B:a-B+n])
u.mem_map(0x60000000,0x10000)
def call(a,args):
 for reg,v in zip([UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3],args):u.reg_write(reg,v)
 u.reg_write(UC_ARM64_REG_LR,0x6000f000);u.reg_write(UC_ARM64_REG_SP,0x6000e000)
 u.emu_start(a,0x6000f000,count=1000)
 assert u.reg_read(UC_ARM64_REG_PC)==0x6000f000
I=0x60000000;O=I+0x100
# Explicit synthetic lookup state: stock table count, pointer; modes zero.
u.mem_write(0xa0281cd58,struct.pack('<I',0x1862));u.mem_write(0xa02f2ab80,struct.pack('<Q',0x431d96d0))
text=[]
for ident in range(0x7e00,0x7f13):
 u.mem_write(I,struct.pack('<I',ident));call(0x4032e70c,[I,O,O+8,O+12])
 p,n,style=struct.unpack('<QII',bytes(u.mem_read(O,16)))
 s=bytes(u.mem_read(p,n)).decode();assert s==f'C-{10+(ident-0x7e00)//5}' and style==0x21f
 text.append(dict(id=hex(ident),string=s,style=hex(style)))
# No installed bitmap resources for owned ids: each falls to stock id zero.
for ident in range(0x7e00,0x7f13):
 u.mem_write(I,struct.pack('<I',ident));call(0x4032dc64,[I,O]);assert bytes(u.mem_read(I,4))==bytes(4)
 assert bytes(u.mem_read(O+16,8))==D[0x431d96e0-B:0x431d96e8-B]
# Existing private bitmap and stock bitmap resolve without replacement.
for ident in [0x45a,0x46a,0x1824,0x7000,0x7014]:
 u.mem_write(I,struct.pack('<I',ident));call(0x4032dc64,[I,O]);assert struct.unpack('<I',bytes(u.mem_read(I,4)))[0]==ident
textids=json.loads(Path('prototype/new-resources.json').read_text());table=Path('prototype/proposed-table.bin').read_bytes();pool=Path('prototype/proposed-png-pool.bin').read_bytes();seen=set()
for j,a in enumerate(textids):
 blob=Path('prototype',a['file']).read_bytes();typ,n,p=struct.unpack_from('<IIQ',table,j*16)
 assert typ==0x100 and n==len(blob) and p%16==0
 assert pool[p-0x40b82000:p-0x40b82000+n]==blob
 im=Image.open(io.BytesIO(blob));im.load();assert im.mode=='L' and im.size==(64,32)
 assert blob[24:29]==bytes([8,0,0,0,0]);seen.add(im.tobytes())
 assert not any(D[p-B:p-B+n])
assert len(seen)==275
for a in json.loads(Path('prototype/original-resources.json').read_text()):
 blob=Path('prototype',a['file']).read_bytes();o=int(a['offset'],16);assert D[o:o+len(blob)]==blob
result=dict(unicorn_text_resolver=275,unicorn_missing_bitmaps_fall_to_id0=275,unicorn_existing_bitmap_smoke=5,prototype_decode_table_and_pixel_uniqueness=275,original_byte_exact=45,limitations='Function emulation with synthetic globals, not full QEMU UI; no candidate code applied.',main_sha256=hashlib.sha256(D).hexdigest())
Path('evidence/offline-checks.json').write_text(json.dumps(result,indent=2)+'\n');print(result)
