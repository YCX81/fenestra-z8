from pathlib import Path
import struct,json,hashlib
R=Path(__file__).resolve().parent
D=R/'nctg-tests/final/host-sample.MOV';d=D.read_bytes();u=lambda p:struct.unpack_from('>I',d,p)[0]
def boxes(lo,hi):
 while lo<hi:
  n,t=struct.unpack_from('>I4s',d,lo);assert n>=8 and lo+n<=hi,(lo,n,hi)
  yield t,lo,n;lo+=n
 assert lo==hi
cont={b'moov',b'trak',b'mdia',b'minf',b'stbl',b'udta',b'NCDT'}
def walk(lo,hi,path=()):
 for t,p,n in boxes(lo,hi):
  yield path+((t,p,n),)
  if t in cont:yield from walk(p+8,p+n,path+((t,p,n),))
nodes=list(walk(0,len(d)));top=list(boxes(0,len(d)));assert [t for t,_,_ in top]==[b'ftyp',b'mdat',b'moov',b'free'];assert d[20:24]==b'niko'
tr=[p for t,p,n in (a[-1] for a in nodes) if t==b'trak']
def find(t,k):return next(a[-1] for a in nodes if a[-1][0]==k and any(x[0]==b'trak' and x[1]==t for x in a))
kind=lambda t:d[find(t,b'hdlr')[1]+16:find(t,b'hdlr')[1]+20]
assert sorted(kind(t) for t in tr)==[b'meta',b'soun',b'vide']
v=next(t for t in tr if kind(t)==b'vide');a=next(t for t in tr if kind(t)==b'soun');m=next(t for t in tr if kind(t)==b'meta')
def samples(t):
 sz=find(t,b'stsz');stride,count=struct.unpack_from('>II',d,sz[1]+12)
 sizes=[stride]*count if stride else list(struct.unpack_from('>'+str(count)+'I',d,sz[1]+20))
 co=find(t,b'stco');offs=list(struct.unpack_from('>'+str(u(co[1]+12))+'I',d,co[1]+16))
 sc=find(t,b'stsc');entries=[struct.unpack_from('>III',d,sc[1]+16+i*12) for i in range(u(sc[1]+12))]
 out=[];chunks=[];idx=0
 for j,off in enumerate(offs,1):
  row=next(e for e in reversed(entries) if e[0]<=j);num=row[1];chunks.append(num)
  for k in range(num):
   assert idx<len(sizes);n=sizes[idx];assert top[1][1]+8<=off and off+n<=top[1][1]+top[1][2];out.append(d[off:off+n]);idx+=1;off+=n
 assert idx==len(sizes)
 return out,chunks,entries,offs
vs,vc,ve,vo=samples(v);aa,ac,ae,ao=samples(a);ms,mc,me,mo=samples(m)
assert vc==[15,15,15,15,15,15,5] and ve==[(1,15,1),(7,5,1)],(vc,ve)
assert max(ac)<=24024 and ac==[24024]*6+[18143],ac
assert ms==[b'\0\0\0\x09\0\0\0\x01\xff']
for sample in vs:
 assert sample[:6]==b'\0\0\0\x03\x46\x01' and sample[6] in (0x10,0x30)
 # no second AUD, preserve every VCL byte from source below
 p=0;types=[]
 while p<len(sample):
  n=int.from_bytes(sample[p:p+4],'big');types.append((sample[p+4]>>1)&63);p+=n+4
 assert p==len(sample) and types.count(35)==1
s16=(R/'input.s16').read_bytes();packed=b''.join(aa);assert len(packed)==len(s16)*3//2
assert packed[::3]==bytes(len(packed)//3)
back=bytearray(len(s16));back[::2]=packed[1::3];back[1::2]=packed[2::3];assert back==s16
sd=find(a,b'stsd');desc=d[sd[1]+16:sd[1]+sd[2]];assert len(desc)==72 and desc[4:8]==b'lpcm' and desc[56:60]==b'\0\0\0\x18'
# Preserve HEVC VCL exactly from the original AU stream.
raw=(R/'input.aus').read_bytes();at=0;orig=[]
while at<len(raw):
 n,pts=struct.unpack_from('<IQ',raw,at);at+=12;au=raw[at:at+n];at+=n
 units=au.split(b'\0\0\0\1')[1:];orig.append([x.rstrip(b'\0') for x in units if ((x[0]>>1)&63)<=31])
for sample,expected in zip(vs,orig):
 p=0;got=[]
 while p<len(sample):
  n=int.from_bytes(sample[p:p+4],'big');unit=sample[p+4:p+4+n]
  if ((unit[0]>>1)&63)<=31:got.append(unit)
  p+=4+n
 assert got==expected
# Current-clip NCDT: fully streamed boxes, actual geometry records, four JPEGs.
ncdt=next((p,n) for t,p,n in (a[-1] for a in nodes) if t==b'NCDT');children={t:(p,n) for t,p,n in boxes(ncdt[0]+8,sum(ncdt))};assert set(children)=={b'NCHD',b'NCTG',b'NCTH',b'NCVW',b'NCM1',b'NCM2',b'NCDB'}
p,n=children[b'NCHD'];assert d[p+8:p+n]==b'Nikon\0\x02\0\0\0';assert children[b'NCDB'][1]==8
p,n=children[b'NCTG'];pos=p+8;tags={}
while pos<p+n:
 tag,typ,count=struct.unpack_from('>IHH',d,pos);size=count*({2:1,3:2,4:4,5:8}[typ]);tags[tag]=(typ,d[pos+8:pos+8+size]);pos+=8+size
assert pos==p+n and set(tags)=={1,2,3,0x13,0x16,0x17,0x18,0x21,0x22,0x23,0x24,0x25,0x26,0x31,0x32,0x33,0x34,0x101,0x102}
assert tags[0x22][1]==struct.pack('>H',1024) and tags[0x23][1]==struct.pack('>H',682)
assert tags[0x101][1]==struct.pack('>4H',160,120,1280,720)
assert tags[0x102][1]==struct.pack('>8H',640,360,1920,1080,0,0,0,0)
assert tags[0x13][1]==struct.pack('>II',95,0)
for tag in [0x16,0x17]:assert tags[tag][1]==struct.pack('>II',30000,1001)
for tag,value in [(0x18,1),(0x21,3),(0x24,1),(0x25,8),(0x26,1),(0x31,1),(0x32,2),(0x33,24),(0x34,48000)]:assert tags[tag][1]==struct.pack('>H',value)
images=[]
for tag,wh in zip((b'NCTH',b'NCVW',b'NCM1',b'NCM2'),((160,120),(1280,720),(640,360),(1920,1080))):
 p,n=children[tag];jpeg=d[p+8:p+n];assert jpeg[:2]==b'\xff\xd8' and jpeg[-2:]==b'\xff\xd9'
 q=2
 while jpeg[q:q+2]!=b'\xff\xc0':q+=2+int.from_bytes(jpeg[q+2:q+4],'big')
 hh,ww=struct.unpack_from('>HH',jpeg,q+5);assert (ww,hh)==wh and jpeg[q+11]==0x21
 out=R/'nctg-tests/final'/(tag.decode()+'.jpg');out.write_bytes(jpeg);images.append(dict(tag=tag.decode(),width=ww,height=hh,bytes=len(jpeg),sha256=hashlib.sha256(jpeg).hexdigest()))
free=top[-1][1];head=int.from_bytes(d[free+14:free+16],'little');num=int.from_bytes(d[free+20:free+24],'little');sizes=struct.unpack_from('<'+str(num)+'I',d,free+32);record=free+8+head+sum(sizes)
assert d[record:record+4]==b'LVJB';off,n,rate=struct.unpack_from('<3I',d,record+4);assert (off,n,rate)==(ao[0],len(aa),48000)
assert b'com.apple.quicktime.content.identifier' in d and b'12345678-1234-1234-1234-123456789ABC' in d
report=dict(status='PASS',sha256=hashlib.sha256(d).hexdigest(),bytes=len(d),video_frames=len(vs),video_chunks=vc,audio_frames=len(aa),audio_chunks=ac,pcm_roundtrip_exact=True,vcl_bytes_preserved=True,apple_metadata_preserved=True,ncdt_jpegs=images,preview_audio='LVJB packed LE24; recovered s16 exact',physical_camera_verified=False)
(R/'nctg-tests/final/host-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
