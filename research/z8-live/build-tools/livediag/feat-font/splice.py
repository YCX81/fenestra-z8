"""v40: port the Live font fix (v37-shutter -> v37-font diff) onto v39 2e48d403,
restamp the bootguard build ID, and write a QEMU changed-ranges manifest vs base c770f340."""
import hashlib,struct,json
from pathlib import Path
H=Path.home();B=0x40160000;BUILD_ID=0x401fe060
sha=lambda b:hashlib.sha256(b).hexdigest()
def runs(a,b,step=4096):
    out=[];n=len(a);i=0
    while i<n:
        if a[i:i+step]==b[i:i+step]:i+=step;continue
        e=min(n,i+step)
        while i<e:
            if a[i]!=b[i]:
                j=i
                while j<n and a[j]!=b[j]:j+=1
                out.append((i,j));i=j
            else:i+=1
    return out
h=(H/'v37build/shutter/v37.main').read_bytes()
s=(H/'v37build/font/v37.main').read_bytes()
c=(H/'fxfix/shutter/q/main.main').read_bytes()
assert sha(c).startswith('2e48d403')
bid=(BUILD_ID-B,BUILD_ID-B+4);ov=lambda a,b:a[0]<b[1] and b[0]<a[1]
rs=[r for r in runs(h,s) if not ov(r,bid)]
rc=runs(h,c)
clash=[(hex(a[0]+B),hex(b[0]+B)) for a in rs for b in rc if ov((a[0]-8,a[1]+8),b) and not ov(b,bid)]
print('shutter runs',len(rs),'v38-vs-hevc runs',len(rc),'clashes',clash[:8]);assert not clash
for a,b in rs: assert c[a:b]==h[a:b],hex(a+B)   # the v38 bytes there are still the hevc bytes
d=bytearray(c)
for a,b in rs:d[a:b]=s[a:b]
assert struct.unpack_from('<Q',d,0x401fe068-B)[0]==0x31544f4f425846
old=struct.unpack_from('<I',d,BUILD_ID-B)[0];struct.pack_into('<I',d,BUILD_ID-B,0)
nid=struct.unpack('<I',hashlib.sha256(bytes(d)).digest()[:4])[0]|0x01000000
struct.pack_into('<I',d,BUILD_ID-B,nid)
d=bytes(d);(H/'fxfix/font/q/main.main').write_bytes(d)
base=(H/'fxfix/base-c770f340.main').read_bytes()
m=json.loads((H/'fxfix/shutter/q/changed-ranges.json').read_text())
lm=json.loads((H/'v37build/font/live/changed-ranges.json').read_text())['symbols']
hm=json.loads((H/'v37build/shutter/live/changed-ranges.json').read_text())['symbols']
syms=dict(m['symbols'])
for k,v in lm.items():
    if 0x401e2000<=v<0x401fe000 or (k in hm and hm[k]!=v) or k not in syms:syms[k]=v
m['symbols']=syms
def merged(rs,gap=4096):
    out=[]
    for a,b in rs:
        if out and a-out[-1][1]<=gap:out[-1]=(out[-1][0],b)
        else:out.append((a,b))
    return out
m['ranges']=[dict(address=hex(a+B),size=b-a,purpose='v40 font overlay vs c770f340',expected=base[a:b].hex(),replacement=d[a:b].hex()) for a,b in merged(runs(base,d))]
m['output_sha256']=sha(d)
(H/'fxfix/font/q/changed-ranges.json').write_text(json.dumps(m,indent=1)+'\n')
json.dump(dict(base='2e48d403',sha256=sha(d),build_id=hex(nid),previous_build_id=hex(old),
  ranges=[[hex(a+B),hex(b+B)] for a,b in rs]),open(H/'fxfix/font/v40.main.json','w'),indent=1)
print('out',sha(d)[:16],'build id',hex(old),'->',hex(nid),'span',hex(min(a for a,_ in rs)+B),hex(max(b for _,b in rs)+B))
