"""Enlarge native GUI object pool 1 (140 x 0x1C0) to 240, storage moved into the slot reservation tail.
Emulation build only."""
import json,hashlib,struct,sys
from pathlib import Path
B=0x40160000
src=Path(sys.argv[1]);out=Path(sys.argv[2])
img=bytearray(src.read_bytes());base_manifest=json.loads((src.parent/'changed-ranges.json').read_text())
N=240;DATA=0xa116f0000;USED=DATA+N*0x1c0
assert USED+N<=0xa11728000
def a64(text,addr):
    from keystone import Ks,KS_ARCH_ARM64,KS_MODE_LITTLE_ENDIAN
    enc,_=Ks(KS_ARCH_ARM64,KS_MODE_LITTLE_ENDIAN).asm(text,addr);return bytes(enc)
ranges=[]
def put(addr,new,purpose,file_off=None):
    off=file_off if file_off is not None else addr-B
    old=bytes(img[off:off+len(new)]);img[off:off+len(new)]=new
    ranges.append(dict(address=hex(off+B),size=len(new),purpose=purpose,expected=old.hex(),replacement=new.hex()))
    print(purpose,hex(off+B),old.hex(),'->',new.hex())
T=0x4B13000+(0xa0282c1a0-0xa02808000)
cnt,cur,size,pad=struct.unpack_from('<HHHH',img,T);d0,u0=struct.unpack_from('<QQ',img,T+8)
assert (cnt,size,d0,u0)==(140,0x1c0,0xa03296950,0xa032a5e50),(cnt,size,hex(d0),hex(u0))
put(None,struct.pack('<HHHHQQ',N,cur,size,pad,DATA,USED),'GUI pool 1: 240 objects in reservation tail',file_off=T)
checks={0x40399dcc:'mov w8, #0x53c',0x40399dd4:'movk w8, #0x8c, lsl #16',0x40399ddc:'mov w0, #0x29e0'}
for a,t in checks.items():assert bytes(img[a-B:a-B+4])==a64(t,a),t
put(0x40399dcc,a64(f'mov w8, #{1200+N:#x}',0x40399dcc),'GUI handle container: total handles 1200+240')
put(0x40399ddc,a64(f'mov w0, #{(1200+N)*8:#x}',0x40399ddc),'GUI handle container: handle array bytes')
m=dict(base_manifest);m['ranges']=base_manifest['ranges']+ranges
m['output_sha256']=hashlib.sha256(img).hexdigest();m['layers']=base_manifest.get('layers',[])+['gui-pool1-240']
out.write_bytes(img);(out.parent/'changed-ranges.json').write_text(json.dumps(m,indent=1))
print('out',m['output_sha256'])
