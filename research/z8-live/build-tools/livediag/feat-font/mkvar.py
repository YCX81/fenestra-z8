import json,hashlib,sys
from pathlib import Path
from keystone import Ks,KS_ARCH_ARM64,KS_MODE_LITTLE_ENDIAN
B=0x40160000;out=Path(sys.argv[1]);out.mkdir(exist_ok=True)
d=bytearray(Path('q/main.main').read_bytes());m=json.loads(Path('q/changed-ranges.json').read_text())
base=Path.home().joinpath('fxfix/base-c770f340.main').read_bytes()
ks=Ks(KS_ARCH_ARM64,KS_MODE_LITTLE_ENDIAN)
for spec in sys.argv[2:]:
    a,old,asm=spec.split('|');A=int(a,16)
    assert d[A-B:A-B+4].hex()==old,(a,d[A-B:A-B+4].hex())
    new=bytes(ks.asm(asm,A)[0]);d[A-B:A-B+4]=new
    hit=[r for r in m['ranges'] if int(r['address'],16)<=A<int(r['address'],16)+r['size']]
    if hit:
        r=hit[0];o=A-int(r['address'],16);rep=bytearray(bytes.fromhex(r['replacement']));rep[o:o+4]=new;r['replacement']=rep.hex()
    else:
        m['ranges'].append(dict(address=hex(A),size=4,purpose='experiment',expected=base[A-B:A-B+4].hex(),replacement=new.hex()));m['ranges'].sort(key=lambda r:int(r['address'],16))
m['output_sha256']=hashlib.sha256(d).hexdigest()
(out/'main.main').write_bytes(d);(out/'changed-ranges.json').write_text(json.dumps(m,indent=1)+'\n');print(out,m['output_sha256'][:12])
