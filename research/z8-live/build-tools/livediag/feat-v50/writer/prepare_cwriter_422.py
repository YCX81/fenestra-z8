"""Native-QEMU fixture: keep C writer layout/audio/images/Apple metadata, use
known 4K video samples so the available display model can finish playback.
The unmodified 1024x682 C writer sample is separately checked in parser tests.
"""
from pathlib import Path
import sys,struct,subprocess,hashlib,json
H=Path(__file__).resolve().parent;sys.path.insert(0,str(H.parents[1]/'feat-mov'))
import mov_fix as m
reference=(H/'parser/LIV_9013.MOV').read_bytes();moov=m.direct_child(m.parse(reference),b'moov')
vt=next(t for t in moov.all(b'trak') if m.handler(t)==b'vide')
hv=vt.one(b'hvcC').data;pos=23;stream=bytearray()
for _ in range(hv[22]):
 count=int.from_bytes(hv[pos+1:pos+3],'big');pos+=3
 for _ in range(count):
  n=int.from_bytes(hv[pos:pos+2],'big');pos+=2;stream+=b'\0\0\0\1'+hv[pos:pos+n];pos+=n
_,offs=m.offsets(vt);sz=vt.one(b'stsz').data;n=m.u32(sz,8);sizes=[m.u32(sz,12+i*4) for i in range(n)]
sc=vt.one(b'stsc').data;ents=[struct.unpack_from('>III',sc,8+12*i) for i in range(m.u32(sc,4))]
idx=ei=0
for ci,off in enumerate(offs,1):
 if ei+1<len(ents) and ents[ei+1][0]==ci:ei+=1
 for _ in range(ents[ei][1]):
  sample=reference[off:off+sizes[idx]];p=0
  while p<len(sample):
   count=int.from_bytes(sample[p:p+4],'big');p+=4;stream+=b'\0\0\0\1'+sample[p:p+count];p+=count
  off+=sizes[idx];idx+=1
assert idx==n==95
(H/'parser/native-video.h265').write_bytes(stream)
out=H/'parser/native-cwriter-422.MOV'
subprocess.run([sys.executable,str(H.parents[1]/'feat-w5dec/replace_video.py'),str(H/'nctg-tests/host-sample.MOV'),str(H/'parser/native-video.h265'),str(out),'3840','2160'],check=True)
# The legacy offline replacement helper knows normal tracks. Fix this C writer's
# private LVJB record too, keeping the REC preview fixture internally consistent.
old=(H/'nctg-tests/host-sample.MOV').read_bytes();data=bytearray(out.read_bytes())
before=m.inspect(old);after=m.inspect(bytes(data))
a0=next(t['first_offset'] for t in before['tracks'] if t['handler']=='soun')
a1=next(t['first_offset'] for t in after['tracks'] if t['handler']=='soun')
free=m.direct_child(m.parse(bytes(data)),b'free');p=data.find(b'LVJB',free.start)
assert p>=0 and int.from_bytes(data[p+4:p+8],'little')==a0
struct.pack_into('<I',data,p+4,a1);out.write_bytes(data)
for kind in ['soun','meta']:
 assert next(t['sample_sha256'] for t in before['tracks'] if t['handler']==kind)==next(t['sample_sha256'] for t in after['tracks'] if t['handler']==kind)
assert next(t['sample_sha256'] for t in m.inspect(reference)['tracks'] if t['handler']=='vide')==next(t['sample_sha256'] for t in after['tracks'] if t['handler']=='vide')
report=dict(scope='Native model fixture: C writer tables/NCDT/Apple/audio; substituted known 4K video samples',input_sha256=hashlib.sha256(old).hexdigest(),output_sha256=hashlib.sha256(data).hexdigest(),bytes=len(data),video_source_sha256=hashlib.sha256(reference).hexdigest(),media=after)
(H/'parser/native-cwriter-422.json').write_text(json.dumps(report,indent=2)+'\n');print(report['bytes'],report['output_sha256'])
