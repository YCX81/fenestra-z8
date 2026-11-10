from pathlib import Path
import struct,json,hashlib,io
from PIL import Image,ImageDraw
B=0x40160000
D=Path('bg34.main').read_bytes()
assert hashlib.sha256(D).hexdigest()=='c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4'
def off(a):return a-B if a<0xA00000000 else a-0xA02808000+0x4b13000
def read(a,n):return D[off(a):off(a)+n]
OUT=Path('prototype');OUT.mkdir(exist_ok=True)
rows=[];thumbs=[]
for v,a in enumerate([0xA028B5DC6,0xA028B5DD8,0xA028B5DEA,0xA028B5E3A,0xA028B5E4C]):
 ids=struct.unpack('<9H',read(a,18));print(v,hex(a),[hex(i) for i in ids])
 for s,i in enumerate(ids,1):
  rec=0x431d96d0+i*24
  kind,size,_,ptr=struct.unpack('<IIQQ',read(rec,24))
  blob=read(ptr,size);im=Image.open(io.BytesIO(blob));im.load()
  f=f'original-C{s:02}-v{v}-id{i:04x}.png';(OUT/f).write_bytes(blob)
  rows.append(dict(slot=s,variant=v,id=hex(i),record=hex(rec),kind=hex(kind),address=hex(ptr),offset=hex(off(ptr)),bytes=size,width=im.width,height=im.height,mode=im.mode,ihdr=blob[24:29].hex(),file=f))
  thumbs.append((s,v,i,im))
canvas=Image.new('RGB',(9*180,5*160),'#303030');dr=ImageDraw.Draw(canvas)
for s,v,i,im in thumbs:
 x=(s-1)*180;y=v*160
 dr.text((x+5,y+3),f'C{s} v{v} ID {i:04x}',fill='white');im=im.convert('RGB'); im.thumbnail((170,125));canvas.paste(im,(x+5,y+25))
canvas.save(OUT/'original-contact.png')
(OUT/'original-resources.json').write_text(json.dumps(rows,indent=2)+'\n')
print(rows[:2])
