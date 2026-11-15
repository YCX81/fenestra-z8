"""Create a QEMU-only N3 experiment; never edits an input image or physical media."""
from pathlib import Path
import hashlib,json,struct,sys
src,out=map(Path,sys.argv[1:3]);assert not out.exists();out.mkdir(parents=True)
b=src.read_bytes();assert hashlib.sha256(b).hexdigest()=='cad9651a1b6517187c068cf432e95a8bff7faa0b7f5795afced359aef0574f00'
d=bytearray(b);B=0x40160000;CAVE=0x4019fc00;HOOK=0x40cf7d08
code=Path(__file__).with_name('crop_fix.bin').read_bytes();assert len(code)==128
assert d[HOOK-B:HOOK-B+4]==bytes.fromhex('692a0079')
assert not any(d[CAVE-B:CAVE-B+len(code)+32])
struct.pack_into('<I',d,HOOK-B,0x14000000|(((CAVE-HOOK)//4)&0x3ffffff))
d[CAVE-B:CAVE-B+len(code)]=code
ID=0x401fe060-B;struct.pack_into('<I',d,ID,0);bid=struct.unpack('<I',hashlib.sha256(d).digest()[:4])[0]|0x01000000;struct.pack_into('<I',d,ID,bid)
r=dict(version='v53n3-QEMU-ONLY',source_sha256=hashlib.sha256(b).hexdigest(),sha256=hashlib.sha256(d).hexdigest(),build_id=hex(bid),hook=hex(HOOK),cave=hex(CAVE),code_bytes=len(code),scope='N2 neighboring-media proof plus bounded initial crop repair for1024x682/1280x852. Not production registration or flash acceptance.',physical_camera_verified=False)
(out/'experiment.main').write_bytes(d);(out/'experiment.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
