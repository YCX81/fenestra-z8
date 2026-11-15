"""Test-only: restore the real Live Apple metadata in the verified 4K playback sample.
This does not generate Nikon metadata or change AV samples; not a production writer.
"""
from pathlib import Path
import sys,copy,struct,json,hashlib
H=Path(__file__).resolve().parent
sys.path.insert(0,str(H.parents[1]/'feat-mov'))
import mov_fix as m
orig=(H.parents[1]/'feat-w5dec/hw-DSC_0028.MOV').read_bytes()
base=(H/'parser/LIV_9013.MOV').read_bytes()
oldmoov=m.direct_child(m.parse(orig),b'moov');roots=m.parse(base);moov=m.direct_child(roots,b'moov')
assert all(m.handler(t)!=b'meta' for t in moov.all(b'trak'))
track=copy.deepcopy(next(t for t in oldmoov.all(b'trak') if m.handler(t)==b'meta'))
co,offs=m.offsets(track);assert len(offs)==1
sample=orig[offs[0]:offs[0]+9];assert sample==bytes.fromhex('0000000900000001ff')
moov.children.extend([copy.deepcopy(m.direct_child(oldmoov.children,b'meta')),track])
start=sum(len(x.encode()) for x in roots)+8
assert co.typ==b'stco';co.data=co.data[:8]+struct.pack('>I',start)
out=b''.join(x.encode() for x in roots)+m.Box(b'mdat',sample).encode()
before=m.inspect(base);after=m.inspect(out)
assert before['tracks']==after['tracks'][:2]
assert after['tracks'][2]['samples']==1
(H/'parser/LIV_9013-META.MOV').write_bytes(out)
(H/'parser/restored-metadata.json').write_text(json.dumps(dict(before=before,after=after),indent=2)+'\n')
print('video/audio byte-identical; metadata restored:',hashlib.sha256(out).hexdigest())
