"""Offline PNG resource proposal; never writes MAIN or talks to hardware.
Glyphs are cropped from firmware, horizontally condensed to fit C + two digits.
No font substitution; no C9 image reused as an extended slot.
"""
from extract import B,D,read,OUT,rows
from PIL import Image,ImageDraw
import struct,io,json,zlib,hashlib

def resource(i):
 r=read(0x431d96d0+i*24,24);n=struct.unpack_from('<I',r,4)[0];p=struct.unpack_from('<Q',r,16)[0]
 b=read(p,n);return b,Image.open(io.BytesIO(b)).convert('L')
def png(im):
 def ch(k,b):return struct.pack('>I',len(b))+k+b+struct.pack('>I',zlib.crc32(k+b))
 raw=b''.join(b'\0'+im.crop((0,y,64,y+1)).tobytes() for y in range(32))
 return b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>II5B',64,32,8,0,0,0,0))+ch(b'IDAT',zlib.compress(raw,9))+ch(b'IEND',b'')
# C is common at x32..47 in non-star native C1. Digits at x48..63.
_,c1=resource(0x45a)
C=c1.crop((32,0,48,32))
glyphs={str(n):resource(0x45a+(n-1)*2)[1].crop((48,0,64,32)) for n in range(1,10)}
# Native creative 20 has a matching zero at x48..63 (ID 0x8db).
glyphs['0']=resource(0x8db)[1].crop((48,0,64,32))
# Use only C1 for five left icon plates; star from C1*, never substitute C9.
baseids=[0x45a,0x1824,0x890,0x45b,0x891]
plates=[resource(i)[1] for i in baseids]
star=plates[3].crop((56,0,64,32))
assets=[];pack=bytearray();table=bytearray();TABLE=0x40b80000;DATA=0x40b82000
for s in range(10,65):
 for v in range(5):
  im=Image.new('L',(64,32));im.paste(plates[v].crop((0,0,32,32)),(0,0))
  strip=Image.new('L',(48,32));strip.paste(C,(0,0));strip.paste(glyphs[str(s)[0]],(16,0));strip.paste(glyphs[str(s)[1]],(32,0))
  width=24 if v>=3 else 32
  # LANCZOS across x only; retain native baseline and height and original left icon.
  im.paste(strip.resize((width,32),Image.Resampling.LANCZOS),(32,0))
  if v>=3:im.paste(star,(56,0))
  blob=png(im);ident=0x7e00+(s-10)*5+v
  filename=f'new-C{s:02}-v{v}-id{ident:04x}.png';(OUT/filename).write_bytes(blob)
  while len(pack)%16:pack.append(0)
  addr=DATA+len(pack);pack.extend(blob)
  table.extend(struct.pack('<IIQ',0x100,len(blob),addr))
  assets.append(dict(slot=s,variant=v,id=hex(ident),file=filename,bytes=len(blob),proposed_va=hex(addr),sha256=hashlib.sha256(blob).hexdigest()))
  assert Image.open(io.BytesIO(blob)).size==(64,32)
  assert Image.open(io.BytesIO(blob)).mode=='L'
  assert im.crop((0,0,32,32)).tobytes()==plates[v].crop((0,0,32,32)).tobytes()
assert len(assets)==275 and len(table)==4400
assert len({x['sha256'] for x in assets})==275
(OUT/'proposed-table.bin').write_bytes(table);(OUT/'proposed-png-pool.bin').write_bytes(pack)
(OUT/'new-resources.json').write_text(json.dumps(assets,indent=2)+'\n')
# Two side-by-side panels, labels explicitly identify stock vs proposed.
W=1430;H=16*160+70
preview=Image.new('RGB',(W,H),'#242424');dr=ImageDraw.Draw(preview)
dr.text((12,12),'STOCK C1-C9 (5 variants, original bytes)',fill='white')
dr.text((445,12),'PROTOTYPE C10-C64 (5 variants per slot; horizontal condensation)',fill='white')
for s in range(1,10):
 y=55+(s-1)*160;dr.text((10,y),f'C{s}',fill='white')
 for v in range(5):
  item=next(r for r in rows if r['slot']==s and r['variant']==v)
  im=Image.open(OUT/item['file']).convert('RGB').resize((128,64),Image.Resampling.NEAREST)
  x=65+(v%2)*170;yy=y+(v//2)*40
  # show at native size to accommodate all five without overlap
  im=Image.open(OUT/item['file']).convert('RGB')
  preview.paste(im,(x,yy));dr.text((x+68,yy+8),f'v{v}',fill='#aaaaaa')
for s in range(10,65):
 col=(s-10)%4;row=(s-10)//4;x=445+col*245;y=55+row*180
 dr.text((x,y),f'C{s}',fill='white')
 for v in range(5):
  a=assets[(s-10)*5+v];im=Image.open(OUT/a['file']).convert('RGB')
  xx=x+35+(v%2)*95;yy=y+(v//2)*44
  preview.paste(im,(xx,yy));dr.text((xx,yy+32),f'v{v}',fill='#aaaaaa')
preview.save(OUT/'comparison-all.png')
# Readable 3x representative comparison.
sel=[1,2,9,10,11,20,32,64];pv=Image.new('RGB',(8*205,5*125+45),'#242424');dr=ImageDraw.Draw(pv)
for col,s in enumerate(sel):
 dr.text((col*205+5,5),f'{"STOCK" if s<10 else "NEW"} C{s}',fill='white')
 for v in range(5):
  item=next(r for r in rows if r['slot']==s and r['variant']==v) if s<10 else assets[(s-10)*5+v]
  im=Image.open(OUT/item['file']).convert('RGB').resize((192,96),Image.Resampling.NEAREST)
  pv.paste(im,(col*205+5,40+v*125));dr.text((col*205+5,25+v*125),f'v{v}',fill='#aaaaaa')
pv.save(OUT/'comparison-detail.png')
summary=dict(original_count=len(rows),new_count=len(assets),png_bytes=sum(a['bytes'] for a in assets),aligned_pool_bytes=len(pack),table_bytes=len(table),proposed_table=hex(TABLE),proposed_pool=hex(DATA),proposed_end=hex(DATA+len(pack)),proposed_copy_end=hex((DATA+len(pack)+4095)&~4095),native_left_32_columns_preserved=True,all_ids_unique=True,all_pixels_unique=True,MAIN_sha256=hashlib.sha256(D).hexdigest(),method='Native C and 1..9 from v0 C1..C9; zero from 0x8db; each variant left plate from C1; text strip scaled in x to 32/24 pixels. Not a native two-digit font or state2 text screenshot.')
(OUT/'validation.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
