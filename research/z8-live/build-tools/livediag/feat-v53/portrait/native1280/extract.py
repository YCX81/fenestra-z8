from pathlib import Path
import struct,json,hashlib
P=Path(__file__).resolve().parent;b=(P/'base.MOV').read_bytes()
def atoms(lo,hi):
 while lo<hi:
  n,t=struct.unpack_from('>I4s',b,lo);assert n>=8 and lo+n<=hi;yield(lo,n,t);lo+=n
moov=next(x for x in atoms(0,len(b))if x[2]==b'moov');trak=next(x for x in atoms(moov[0]+8,sum(moov[:2]))if x[2]==b'trak')
def child(x,t):return next(z for z in atoms(x[0]+8,x[0]+x[1])if z[2]==t)
stbl=child(child(child(trak,b'mdia'),b'minf'),b'stbl');a={t:(o,n)for o,n,t in atoms(stbl[0]+8,stbl[0]+stbl[1])}
z,n=a[b'stsz'];cnt=struct.unpack_from('>I',b,z+16)[0];sizes=struct.unpack_from('>'+str(cnt)+'I',b,z+20)
z,n=a[b'stco'];cc=struct.unpack_from('>I',b,z+12)[0];offsets=struct.unpack_from('>'+str(cc)+'I',b,z+16)
z,n=a[b'stsc'];sc=struct.unpack_from('>'+str((n-16)//4)+'I',b,z+16)
z,n=a[b'stss'];sync=struct.unpack_from('>'+str((n-16)//4)+'I',b,z+16)
# Decode hvcC exactly as stored, then all media AUs (AUD remains untouched).
z=b.index(b'hvcC',a[b'stsd'][0],a[b'stsd'][0]+a[b'stsd'][1])+4;h=z;arr=b[h+22];z=h+23;sets=[]
for i in range(arr):
 ty=b[z]&63;nn=struct.unpack_from('>H',b,z+1)[0];z+=3
 for j in range(nn):
  ln=struct.unpack_from('>H',b,z)[0];z+=2;sets.append(b[z:z+ln]);z+=ln
annex=bytearray().join(b'\0\0\0\1'+p for p in sets);frames=[];pos=offsets[0];out_start=[]
for i,n in enumerate(sizes):
 nals=[];end=pos+n;out_start.append(len(annex))
 while pos<end:
  ln=struct.unpack_from('>I',b,pos)[0];pos+=4;nal=b[pos:pos+ln];assert ln and pos+ln<=end;nals.append((nal[0]>>1&63,ln));annex.extend(b'\0\0\0\1'+nal);pos+=ln
 frames.append(dict(index=i,sync=i+1 in sync,bytes=n,nals=nals,annex_end=len(annex)))
(P/'stream.h265').write_bytes(annex)
r=dict(source_sha256=hashlib.sha256(b).hexdigest(),bytes=len(b),count=cnt,stsc=list(sc),stco=list(offsets),stss=list(sync),parameter_sets=[dict(type=x[0]>>1&63,bytes=len(x))for x in sets],media_nals=frames,annexb_sha256=hashlib.sha256(annex).hexdigest())
(P/'structure.json').write_text(json.dumps(r,indent=2)+'\n');print('wrote',cnt,'AU, AUD each',all(x['nals'][0]==(35,3)for x in frames))
