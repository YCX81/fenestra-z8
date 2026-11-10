from pathlib import Path
import struct,json,hashlib
B=0x40160000;D=Path('bg34.main').read_bytes()
def off(a):return a-B if a<0xa00000000 else 0x4b13000+a-0xa02808000
def rd(a,n):return D[off(a):off(a)+n]
a=0x40b20200;sp=[]
while True:
 x,n=struct.unpack('<II',rd(a,8));a+=8
 if not n:break
 sp.append((x,n))
buf=bytearray(0x100000);maps=[]
for x,n in sp:buf[x:x+n]=rd(a,n);maps.append((x,n,a));a+=n
Path('prototype/boot-spans.json').write_text(json.dumps(maps,indent=2)+'\n')
labels=[]
for j in range(275):
 p,n,style,res=struct.unpack_from('<QIII',buf,0xa5200+j*32)
 value=bytes(buf[p-0xa1162b000:p-0xa1162b000+n]).decode()
 assert value==f'C-{10+j//5}' and style==0x21f
 labels.append(dict(id=hex(0x7e00+j),pointer=hex(p),length=n,style=hex(style),value=value))
strings=[]
for j in range(9):
 a0=0x42afbd18+j*16;k,p=struct.unpack('<QQ',rd(a0,16));strings.append(dict(table=hex(a0),id=hex(k),pointer=hex(p),text=rd(p,4).split(b'\0')[0].decode()))
private=[]
for j in range(21):
 typ,n,p=struct.unpack('<IIQ',rd(0x40a41400+j*16,16));w,h=struct.unpack('>II',rd(p+16,8));private.append(dict(id=hex(0x7000+j),kind=hex(typ),bytes=n,ptr=hex(p),size=[w,h]))
rects=[]
for a0,count in [(0xa028dea48,5),(0xa028dea68,3),(0xa028deb28,5),(0xa028deb48,3)]:
 ptr=struct.unpack('<Q',rd(a0+16,8))[0];rects.append(dict(layout=hex(a0),table=hex(ptr),rectangles=[struct.unpack('<4H',rd(ptr+j*8,8)) for j in range(count)]))
ids={0x7e00+j for j in range(275)};remap=[]
for a0,count in [(0x40a41550,51),(0x40a41620,27)]:
 pairs=[struct.unpack('<HH',rd(a0+j*4,4)) for j in range(count)];assert not ids.intersection(v for pair in pairs for v in pair);remap.append(dict(address=hex(a0),pairs=pairs))
assert not any(rd(0x40b80000,0x40000))
result=dict(main_sha256=hashlib.sha256(D).hexdigest(),boot_payload_end=hex(a),strings=strings,extended_text=labels,styles={hex(i):rd(0x43a0d540+i*16,16).hex() for i in (0x21f,0x229)},label_rectangles=rects,existing_private_images=private,remaps=remap,proposed_40b80000_40bc0000_all_zero=True,driver_sha256=hashlib.sha256(Path('run_imenu.reference.py').read_bytes()).hexdigest())
Path('evidence/static-audit.json').write_text(json.dumps(result,indent=2)+'\n');print('275 text descriptors, 9 native strings, 21 private PNGs, 4 label layouts verified')
