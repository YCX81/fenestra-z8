"""v41d DIAGNOSTIC build (not for normal use) = v41 ee11bb57 +
 1. fx_z8_menu_ptp_active 0x401cc3f0: ignore shutdown / startup.shutdown, so the Fenestra USB debug channel
    stays up after a Panic (only activation is required);
 2. fx_native_module_fault 0x401cc43c entry -> cave 0x401fef00: records up to 4 calls (x30 caller, x0 owner, x1 error)
    at RAM 0xa1171b080 (u32 count, then 24-byte records), then runs the relocated stp and continues;
 3. bootguard build ID restamped."""
import hashlib,json,struct
from pathlib import Path
from keystone import Ks,KS_ARCH_ARM64,KS_MODE_LITTLE_ENDIAN
H=Path.home();B=0x40160000;BUILD_ID=0x401fe060
ks=Ks(KS_ARCH_ARM64,KS_MODE_LITTLE_ENDIAN)
asm=lambda s,a:bytes(ks.asm(s,a)[0])
d=bytearray((H/'fxfix/v41/q/main.main').read_bytes())
assert hashlib.sha256(d).hexdigest().startswith('ee11bb57')
def put(a,old,new):
    assert d[a-B:a-B+len(old)]==old,(hex(a),d[a-B:a-B+len(old)].hex());d[a-B:a-B+len(new)]=new
# 1. ptp active
put(0x401cc414,bytes.fromhex('68000034'),asm('b 0x401cc420',0x401cc414))
put(0x401cc430,bytes.fromhex('e0179f1a'),asm('mov w0,#1',0x401cc430))
# 2. fault recorder
CAVE=0x401fef00
assert not any(d[CAVE-B:CAVE-B+0x100])
code=f"""
movz x16,#0xb080
movk x16,#0x1171,lsl #16
movk x16,#0xa,lsl #32
ldr w9,[x16]
cmp w9,#4
b.hs skip
add x10,x16,#8
add x11,x9,x9,lsl #1
add x10,x10,x11,lsl #3
stp x30,x0,[x10]
str x1,[x10,#16]
add w9,w9,#1
str w9,[x16]
skip:
stp x29,x30,[sp,#-0x20]!
b 0x401cc440
"""
cave=asm(code,CAVE);d[CAVE-B:CAVE-B+len(cave)]=cave
put(0x401cc43c,bytes.fromhex('fd7bbea9'),asm(f'b {CAVE:#x}',0x401cc43c))
# 3. build id
old=struct.unpack_from('<I',d,BUILD_ID-B)[0];struct.pack_into('<I',d,BUILD_ID-B,0)
nid=struct.unpack('<I',hashlib.sha256(bytes(d)).digest()[:4])[0]|0x01000000;struct.pack_into('<I',d,BUILD_ID-B,nid)
out=H/'fxfix/v41d/q';out.mkdir(parents=True,exist_ok=True);d=bytes(d);(out/'main.main').write_bytes(d)
stock=(H/'fxfix/base-c770f340.main').read_bytes();m=json.loads((H/'fxfix/v41/q/changed-ranges.json').read_text())
def runs(a,b,step=4096):
    o=[];n=len(a);i=0
    while i<n:
        if a[i:i+step]==b[i:i+step]:i+=step;continue
        e=min(n,i+step)
        while i<e:
            if a[i]!=b[i]:
                j=i
                while j<n and a[j]!=b[j]:j+=1
                o.append((i,j));i=j
            else:i+=1
    return o
o=[]
for a,b in runs(stock,d):
    if o and a-o[-1][1]<=4096:o[-1]=(o[-1][0],b)
    else:o.append((a,b))
m['ranges']=[dict(address=hex(a+B),size=b-a,purpose='v41d diag vs c770f340',expected=stock[a:b].hex(),replacement=d[a:b].hex()) for a,b in o]
m['output_sha256']=hashlib.sha256(d).hexdigest();(out/'changed-ranges.json').write_text(json.dumps(m,indent=1)+'\n')
json.dump(dict(base='ee11bb57',sha256=m['output_sha256'],build_id=hex(nid),previous_build_id=hex(old),cave=len(cave)),open(out.parent/'v41d.main.json','w'),indent=1)
print(m['output_sha256'][:16],hex(nid),len(cave))
