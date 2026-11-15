"""Compose a private v53 candidate with checked, nonoverlapping overlays.

This does not package an updater or enable the experimental native REC bridge.
"""
from pathlib import Path
import hashlib,json,struct,subprocess,sys
BUILD,OUT=map(Path,sys.argv[1:3])
ROOT=Path(__file__).resolve().parent
V52=Path('/path/to/user/fxfix/v52-root')
BASE=0x40160000;ID=0x401fe060-BASE
sha=lambda b:hashlib.sha256(b).hexdigest()
assert not OUT.exists();OUT.mkdir(parents=True)
subprocess.run([sys.executable,str(ROOT/'compose_base53.py'),str(BUILD),str(OUT/'v52-overlays')],check=True)
reference=(V52/'v52g/q/main.main').read_bytes()
assert sha(reference)=='ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
sys.path.insert(0,str(ROOT/'portrait'))
from portrait53 import apply
old=(OUT/'v52-overlays/main.main').read_bytes()
image,edits=apply(old,reference);image=bytearray(image)
sys.path.insert(0,str(ROOT))
from split_live_payload_v53 import split
(primary,extra,shutter),symbols,sections=split(BUILD/'live/payload.elf')
for address,payload in [(0x401e2000,primary),(0x401fd218,extra),(0x4019f020,shutter)]:
    assert image[address-BASE:address-BASE+len(payload)]==payload,hex(address)
assert len(shutter)<=0x4019ff40-0x4019f020

def branch(source,target):
    delta=target-source
    assert delta%4==0 and -(1<<27)<=delta<(1<<27)
    return struct.pack('<I',0x14000000|((delta//4)&0x3ffffff))

overlays=[]
def overlay(address,expected,replacement,purpose):
    assert len(expected)==len(replacement)
    at=address-BASE
    assert image[at:at+len(expected)]==expected,(hex(address),purpose)
    image[at:at+len(replacement)]=replacement
    overlays.append(dict(address=hex(address),size=len(replacement),purpose=purpose,
                         expected=expected.hex(),replacement=replacement.hex()))

target=symbols['live_shutter_dispatch']
assert 0x4019f020<=target<0x4019ff40
overlay(0x40e30e3c,bytes.fromhex('3c0f0014'),branch(0x40e30e3c,target),
        'Original SFX owner dispatch with reviewed microphone handoff')
target=symbols['live_ui_evf_submit']
assert 0x401e2000<=target<0x401fd000 or 0x401fd218<=target<0x401fe000
overlay(0x401def00,struct.pack('<Q',0x403efed8),struct.pack('<Q',target),
        'Passive EVF submit observation; no scanout change')
for name,address,size,hook,expected in [
    ('crop_fix',0x4019ff40,128,0x40cf7d08,bytes.fromhex('692a0079')),
    ('aspect_fix',0x4019ffc0,64,0x407138d0,branch(0x407138d0,0x409708f0))]:
    payload=(ROOT/'native-rec/crop-fix'/f'{name}.bin').read_bytes()
    assert len(payload)==size
    # These assembly fragments have only local PC-relative references and
    # absolute native literals. The original NFP aspect wrapper is preserved.
    overlay(address,bytes(size),payload,f'Bounded native Live {name}')
    overlay(hook,expected,branch(hook,address),f'Native {name} entry')
assert sha((ROOT/'native-rec/crop-fix/aspect_fix.bin').read_bytes())=='87678087a29c0389a4342651ae7ee0162e5c664eb25355b989d2e452fc08f593'
struct.pack_into('<I',image,ID,0)
build_id=struct.unpack('<I',hashlib.sha256(image).digest()[:4])[0]|0x01000000
struct.pack_into('<I',image,ID,build_id)
assert image[0x401fd000-BASE:0x401fd218-BASE]==reference[0x401fd000-BASE:0x401fd218-BASE]
final=OUT/'q';final.mkdir();(final/'main.main').write_bytes(image)
manifest=json.loads((OUT/'v52-overlays/changed-ranges.json').read_text())
stock=Path('/path/to/user/fxfix/base-c770f340.main').read_bytes()
ranges=[];start=None
for i,(a,b) in enumerate(zip(stock,image)):
    if a!=b:
        if start is None:start=i
        end=i+1
    elif start is not None and i-end>4096:
        ranges.append((start,end));start=None
if start is not None:ranges.append((start,end))
manifest['ranges']=[dict(address=hex(BASE+a),size=b-a,purpose='v53 aggregate overlay',
    expected=stock[a:b].hex(),replacement=image[a:b].hex()) for a,b in ranges]
manifest['output_sha256']=sha(image)
(final/'changed-ranges.json').write_text(json.dumps(manifest,indent=1)+'\n')
receipt=json.loads((OUT/'v52-overlays/main.json').read_text())
receipt.update(version=OUT.name+'-private-candidate',sha256=sha(image),build_id=hex(build_id),
    v52_composition_sha256=sha(old),portrait53_records=len(edits),physical_camera_verified=False,
    overlays53=overlays,
    scope='REC audio routing and bounded buffers; microphone startup and shutter handoff; aspect fit; portrait recording layout; native crop/aspect; passive EVF diagnostic. Native REC bridge and physical EVF icon fix not implemented.')
(final/'main.json').write_text(json.dumps(receipt,indent=2)+'\n')
(final/'portrait53-ranges.json').write_text(json.dumps(edits,indent=2)+'\n')
print(json.dumps(receipt,indent=2))
