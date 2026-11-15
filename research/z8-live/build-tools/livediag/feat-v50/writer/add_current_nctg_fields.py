"""Offline diagnostic: add factual fixed-profile scalar fields to C writer NCTG."""
from pathlib import Path
import sys,struct,json,hashlib
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'feat-mov'))
import mov_fix as m

def span(d,lo,hi,t):
 p=lo;out=[]
 while p<hi:
  n,k=struct.unpack_from('>I4s',d,p);assert n>=8 and p+n<=hi
  if k==t:out.append((p,n))
  p+=n
 assert p==hi and len(out)==1,(t,len(out));return out[0]
def records(d):
 out={};p=0;width=(0,1,1,2,4,8,1,1,2,4,8)
 while p<len(d):
  t,k,c=struct.unpack_from('>IHH',d,p);n=8+c*width[k];assert p+n<=len(d);out[t]=d[p:p+n];p+=n
 return out
def record(t,k,c,x):return struct.pack('>IHH',t,k,c)+x
def sha(d):return hashlib.sha256(d).hexdigest()
def make(source,dest):
 d=source.read_bytes();ins=m.inspect(d);tracks=ins['tracks'];video=next(x for x in tracks if x['handler']=='vide');audio=next((x for x in tracks if x['handler']=='soun'),None)
 root=m.direct_child(m.parse(d),b'moov');vt=next(t for t in root.all(b'trak') if m.handler(t)==b'vide');hv=vt.one(b'hvcC').data
 assert hv[16]&3==1 and hv[17]&7==0 and hv[18]&7==0,'expected current HEVC 8-bit 4:2:0 profile'
 dims=video['hvc1_size'];assert dims==video['sps']['display']
 if audio:
  at=next(t for t in root.all(b'trak') if m.handler(t)==b'soun');sd=at.one(b'stsd').data
  assert b'lpcm' in sd and struct.pack('>I',24) in sd,'expected current packed24 PCM'
 ranges=[];lo,hi=0,len(d)
 for name in [b'moov',b'udta',b'NCDT',b'NCTG']:
  p,n=span(d,lo,hi,name);ranges.append((name,p,n));lo,hi=p+8,p+n
 tags=records(d[lo:hi]);original=set(tags)
 tags[0x13]=record(0x13,4,2,struct.pack('>II',video['samples'],0))
 for t in [0x16,0x17]:tags[t]=record(t,5,1,struct.pack('>II',30000,1001))
 for t,v in [(0x18,1),(0x21,3),(0x24,1),(0x25,8),(0x26,1),(0x31,1 if audio else 0),(0x32,2 if audio else 0),(0x33,24 if audio else 0),(0x34,48000 if audio else 0)]:tags[t]=record(t,3,1,struct.pack('>H',v))
 body=b''.join(tags[t] for t in sorted(tags));newbox=struct.pack('>I4s',len(body)+8,b'NCTG')+body
 out=bytearray(d[:p]+newbox+d[p+n:]);delta=len(newbox)-n
 for name,a,size in ranges[:-1]:struct.pack_into('>I',out,a,size+delta)
 after=m.inspect(bytes(out));assert tracks==after['tracks'] and ins['mdat_sha256']==after['mdat_sha256']
 # Undo size words and the sole replacement; every other byte must round-trip.
 reverted=bytearray(out[:p]+d[p:p+n]+out[p+len(newbox):])
 for _,a,size in ranges[:-1]:struct.pack_into('>I',reverted,a,size)
 assert reverted==d
 dest.write_bytes(out)
 report=dict(source=str(source.resolve()),source_sha256=sha(d),output_sha256=sha(out),bytes=len(out),frame_count=video['samples'],geometry=dims,nominal_fps=[30000,1001],audio_present=bool(audio),new_tags=[hex(t) for t in sorted(set(tags)-original)],writes=[dict(atom=name.decode(),offset=a,before_bytes=size,after_bytes=size+delta,operation='replace entire NCTG' if name==b'NCTG' else 'rewrite 4-byte length') for name,a,size in ranges],reversible_exact=True,all_tracks_and_mdat_exact=True)
 dest.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':make(Path(sys.argv[1]),Path(sys.argv[2]))
