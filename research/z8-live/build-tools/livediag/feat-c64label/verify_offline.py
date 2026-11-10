from pathlib import Path
import hashlib,struct,json,io,zlib
from PIL import Image
B=0x40160000;D=Path('bg34.main').read_bytes()
textids=json.loads(Path('prototype/new-resources.json').read_text());table=Path('prototype/proposed-table.bin').read_bytes();pool=Path('prototype/proposed-png-pool.bin').read_bytes();seen=set()
for j,a in enumerate(textids):
 blob=Path('prototype',a['file']).read_bytes();typ,n,p=struct.unpack_from('<IIQ',table,j*16)
 assert typ==0x100 and n==len(blob) and p%16==0
 assert pool[p-0x40b82000:p-0x40b82000+n]==blob
 im=Image.open(io.BytesIO(blob));im.load();assert im.mode=='L' and im.size==(64,32)
 assert blob[24:29]==bytes([8,0,0,0,0]);seen.add(im.tobytes())
 assert not any(D[p-B:p-B+n])
assert len(seen)==275
for a in json.loads(Path('prototype/original-resources.json').read_text()):
 blob=Path('prototype',a['file']).read_bytes();o=int(a['offset'],16);assert D[o:o+len(blob)]==blob
result=dict(unicorn_attempt="Process exited 132 (SIGILL), no execution result accepted",prototype_decode_table_and_pixel_uniqueness=275,original_byte_exact=45,limitations='Static asset checks only; Unicorn process failed; no QEMU UI or candidate code execution.',main_sha256=hashlib.sha256(D).hexdigest())
Path('evidence/offline-checks.json').write_text(json.dumps(result,indent=2)+'\n');print(result)
