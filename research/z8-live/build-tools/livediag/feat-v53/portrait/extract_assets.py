from pathlib import Path
from PIL import Image,ImageDraw
import struct,io,json
P=Path(__file__).parent;D=(P/'v52g.main').read_bytes();B=0x40160000
out=[];imgs=[]
for ident in range(0x3000):
 o=0x431d96d0-B+24*ident
 kind,n,unk,ptr=struct.unpack_from('<IIQQ',D,o)
 p=ptr-B
 if not(0<=p<len(D)-n) or D[p:p+8]!=b'\x89PNG\r\n\x1a\n':continue
 w,h=struct.unpack_from('>II',D,p+16)
 if (w,h) not in ((68,36),(72,36),(86,36),(120,28),(76,28)):continue
 im=Image.open(io.BytesIO(D[p:p+n])).convert('RGBA');bbox=im.getchannel('A').getbbox()
 out.append(dict(id=hex(ident),size=[w,h],bbox=bbox,pointer=hex(ptr)))
 imgs.append((ident,im))
for start in range(0,len(imgs),160):
 batch=imgs[start:start+160];canvas=Image.new('RGB',(8*160,((len(batch)+7)//8)*65),'#202020');dr=ImageDraw.Draw(canvas)
 for j,(ident,im) in enumerate(batch):
  x=j%8*160;y=j//8*65;dr.text((x+3,y+2),hex(ident),fill='yellow');canvas.paste(im,(x+3,y+20),im)
 canvas.save(P/f'assets-{start:04d}.png')
(P/'assets.json').write_text(json.dumps(out,indent=2));print(len(imgs))
