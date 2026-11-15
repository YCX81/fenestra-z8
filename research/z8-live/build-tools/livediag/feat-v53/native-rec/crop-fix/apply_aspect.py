"""Add the reviewed native3:2 selection to the private N3 experiment."""
from pathlib import Path
import hashlib,json,struct,sys
src,out=map(Path,sys.argv[1:3]);assert not out.exists();out.mkdir(parents=True)
b=src.read_bytes();assert hashlib.sha256(b).hexdigest()=='f78221ee194c928cde4a52c8c5b363eac48b5bd4da57f0ecdfb99e8eb032b953'
d=bytearray(b);B=0x40160000;CAVE=0x4019fc80;HOOK=0x407138d0;NFP=0x409708f0
code=Path(__file__).with_name('aspect_fix.bin').read_bytes();assert hashlib.sha256(code).hexdigest()=='87678087a29c0389a4342651ae7ee0162e5c664eb25355b989d2e452fc08f593'
assert struct.unpack_from('<I',d,HOOK-B)[0]==0x14000000|(((NFP-HOOK)//4)&0x3ffffff)
assert not any(d[CAVE-B:CAVE-B+len(code)+32]);d[CAVE-B:CAVE-B+len(code)]=code
struct.pack_into('<I',d,HOOK-B,0x14000000|(((CAVE-HOOK)//4)&0x3ffffff))
ID=0x401fe060-B;struct.pack_into('<I',d,ID,0);bid=struct.unpack('<I',hashlib.sha256(d).digest()[:4])[0]|0x01000000;struct.pack_into('<I',d,ID,bid)
r=dict(version='v53n4-QEMU-ONLY',source_sha256=hashlib.sha256(b).hexdigest(),sha256=hashlib.sha256(d).hexdigest(),build_id=hex(bid),hook=hex(HOOK),cave=hex(CAVE),code_bytes=len(code),scope='N3 plus original3:2 aspect mode only for the two Live resolutions; original NFP wrapper preserved. Private neighboring-media bridge, not a production writer.',physical_camera_verified=False)
(out/'experiment.main').write_bytes(d);(out/'experiment.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
