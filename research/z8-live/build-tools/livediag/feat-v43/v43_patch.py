"""v43 = v42 (1fbb7deb) + Live HEVC parameter-set capacity fix (hw Wave5 SPS is 415 B with scaling lists;
live_hevc vps/sps/pps/sps_raw 128/256/128/256 -> 256/1024/1024/1024, SPS_LIMIT 1024, NAL limit = buffer size,
live_mov sps/pps 256 -> 1024, AU headroom 4 KiB). Splices the Live layer diff (v37 v41 build -> v37 v43 build)
onto v42; every spliced byte must still equal the v41 build in v42. Bootguard build ID restamped."""
import hashlib,json,struct
from pathlib import Path
H=Path.home();B=0x40160000;BUILD_ID=0x401fe060
sha=lambda b:hashlib.sha256(b).hexdigest()
base=bytearray((H/'fxfix/v42/q/main.main').read_bytes());assert sha(base).startswith('1fbb7deb')
old=(H/'v37build/v41/v37.main').read_bytes();new=(H/'v37build/v43/v37.main').read_bytes()
assert sha(old).startswith('3d7abfe2')
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
bid=(BUILD_ID-B,BUILD_ID-B+4);ov=lambda a,b:a[0]<b[1] and b[0]<a[1]
rs=[r for r in runs(old,new) if not ov(r,bid)]
for a,b in rs:
    assert base[a:b]==old[a:b],'later layer owns %#x'%(a+B);base[a:b]=new[a:b]
outside=[(hex(a+B),b-a) for a,b in rs if not (0x40196000<=a+B<0x401fe000)]
oid=struct.unpack_from('<I',base,BUILD_ID-B)[0];struct.pack_into('<I',base,BUILD_ID-B,0)
nid=struct.unpack('<I',hashlib.sha256(bytes(base)).digest()[:4])[0]|0x01000000;struct.pack_into('<I',base,BUILD_ID-B,nid)
d=bytes(base);out=H/'fxfix/v43';(out/'q').mkdir(parents=True,exist_ok=True);(out/'q/main.main').write_bytes(d)
stock=(H/'fxfix/base-c770f340.main').read_bytes()
m=json.loads((H/'fxfix/v42/q/changed-ranges.json').read_text())
lm=json.loads((H/'v37build/v43/live/changed-ranges.json').read_text())['symbols']
for k,v in lm.items():m['symbols'][k]=v
o=[]
for a,b in runs(stock,d):
    if o and a-o[-1][1]<=4096:o[-1]=(o[-1][0],b)
    else:o.append((a,b))
m['ranges']=[dict(address=hex(a+B),size=b-a,purpose='v43 overlay vs c770f340',expected=stock[a:b].hex(),replacement=d[a:b].hex()) for a,b in o]
m['output_sha256']=sha(d);(out/'q/changed-ranges.json').write_text(json.dumps(m,indent=1)+'\n')
info=dict(base='1fbb7deb',sha256=sha(d),build_id=hex(nid),previous_build_id=hex(oid),live_runs=len(rs),outside_live_pb=outside,new_v37_sha256=sha(new))
(out/'v43.main.json').write_text(json.dumps(info,indent=1)+'\n')
print(json.dumps(info,indent=1))
