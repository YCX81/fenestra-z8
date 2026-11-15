from pathlib import Path
import struct,json,hashlib
R=Path(__file__).resolve().parent
src=R.parents[1]/'feat-w5dec/hw-DSC_0028.MOV'
d=src.read_bytes()
def boxes(lo,hi):
 while lo<hi:
  n,t=struct.unpack_from('>I4s',d,lo);assert n>=8 and lo+n<=hi,(lo,n,hi)
  yield t,lo,n
  lo+=n
 assert lo==hi
cont={b'moov',b'trak',b'mdia',b'minf',b'stbl'}
def walk(lo,hi,path=()):
 for t,p,n in boxes(lo,hi):
  yield path+((t,p,n),)
  if t in cont:yield from walk(p+8,p+n,path+((t,p,n),))
nodes=list(walk(0,len(d)))
traks=[a[-1][1] for a in nodes if a[-1][0]==b'trak']
def find(t,k):return next(a[-1] for a in nodes if a[-1][0]==k and any(x[0]==b'trak' and x[1]==t for x in a))
def u32(n,at,count=1):return struct.unpack_from('>'+str(count)+'I',d,n[1]+at)
kind=lambda t:d[find(t,b'hdlr')[1]+16:find(t,b'hdlr')[1]+20]
v=next(t for t in traks if kind(t)==b'vide');a=next(t for t in traks if kind(t)==b'soun')
sd=find(v,b'stsd');entry=sd[1]+16;assert d[entry+4:entry+8]==b'hvc1'
w,h=struct.unpack_from('>HH',d,entry+32)
hv=next((p,n) for t,p,n in boxes(entry+86,entry+struct.unpack_from('>I',d,entry)[0]) if t==b'hvcC')
hv=d[hv[0]+8:hv[0]+hv[1]];pos=23;sets=b''
for _ in range(hv[22]):
 typ=hv[pos]&63;cnt=int.from_bytes(hv[pos+1:pos+3],'big');pos+=3
 for i in range(cnt):
  n=int.from_bytes(hv[pos:pos+2],'big');pos+=2;sets+=b'\0\0\0\1'+hv[pos:pos+n];pos+=n
sz=find(v,b'stsz');count=u32(sz,16)[0];sizes=u32(sz,20,count)
co=find(v,b'stco');offs=u32(co,16,u32(co,12)[0]);assert len(offs)==count
stts=find(v,b'stts');dur=[]
for i in range(u32(stts,12)[0]):
 n,dt=u32(stts,16+8*i,2);dur.extend([dt]*n)
md=find(v,b'mdhd');scale=u32(md,20)[0];assert scale==1000000
out=bytearray();pts=1000000
for i,(at,n) in enumerate(zip(offs,sizes)):
 body=d[at:at+n];p=0;au=sets if i==0 else b''
 while p<n:
  z=int.from_bytes(body[p:p+4],'big');p+=4;assert p+z<=n
  au+=b'\0\0\0\1'+body[p:p+z];p+=z
 out+=struct.pack('<IQ',len(au),pts)+au;pts+=dur[i]
(R/'input.aus').write_bytes(out)
asz=find(a,b'stsz');stride,frames=u32(asz,12,2);assert stride==4
try:
 aco=find(a,b'co64');base=struct.unpack_from('>Q',d,aco[1]+16)[0]
except StopIteration:
 aco=find(a,b'stco');base=u32(aco,16)[0]
(R/'input.s16').write_bytes(d[base:base+frames*4])
# Current clip's own embedded 1024x682 JPEG, no donor thumbnail.
assert d[-4:]==b'LVJT';free=len(d)-int.from_bytes(d[-8:-4],'little');assert d[free+8:free+12]==b'LVJ1'
head=int.from_bytes(d[free+14:free+16],'little');pf=int.from_bytes(d[free+20:free+24],'little')
psizes=struct.unpack_from('<'+str(pf)+'I',d,free+32);p=free+8+head+sum(psizes[:pf//2]);jpeg=d[p:p+psizes[pf//2]]
assert jpeg[:2]==b'\xff\xd8' and jpeg[-2:]==b'\xff\xd9';(R/'input-preview.jpg').write_bytes(jpeg)
j=dict(source=str(src),sha256=hashlib.sha256(d).hexdigest(),width=w,height=h,frames=count,audio_frames=frames,last_duration=dur[-1],preview_index=pf//2,preview_jpeg_sha256=hashlib.sha256(jpeg).hexdigest())
(R/'input.json').write_text(json.dumps(j,indent=2)+'\n');print(j)
