"""v46r = v46 (9be24e70) + Live full release while a stock Wave5 client (movie playback/recording) runs
(runtime.c: bring-up hook sets a hold, worker treats it as ineligible until photo live view returns)
+ Live playback caves (liveplan 0x401fd000, livefbh 0x401fd100). Splices the Live layer diff
(v37 v46 build -> v37 v46r build) onto v46; every spliced byte must still equal the v46 build in v46."""
import hashlib,json,struct,sys
from pathlib import Path
H=Path.home();B=0x40160000;BUILD_ID=0x401fe060
sys.path.insert(0,str(H/'fxfix/liveplay'));import apply_liveplan
sha=lambda b:hashlib.sha256(b).hexdigest()
base=bytearray((H/'fxfix/v46/q/main.main').read_bytes());assert sha(base).startswith('9be24e70')
old=(H/'v37build/v46/v37.main').read_bytes();new=(H/'v37build/v46r/v37.main').read_bytes()
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
assert max(b for a,b in rs if a+B<0x401fe000)+B<=0x401fd000-64,'live payload reaches the caves'
base=apply_liveplan.apply(base)
oid=struct.unpack_from('<I',base,BUILD_ID-B)[0];struct.pack_into('<I',base,BUILD_ID-B,0)
nid=struct.unpack('<I',hashlib.sha256(bytes(base)).digest()[:4])[0]|0x01000000;struct.pack_into('<I',base,BUILD_ID-B,nid)
d=bytes(base);out=H/'fxfix/liveplay46r';(out/'q/main.main').write_bytes(d)
stock=(H/'fxfix/base-c770f340.main').read_bytes()
m=json.loads((H/'fxfix/v46/q/changed-ranges.json').read_text())
lm=json.loads((H/'v37build/v46r/live/changed-ranges.json').read_text())['symbols']
for k,v in lm.items():m['symbols'][k]=v
o=[]
for a,b in runs(stock,d):
    if o and a-o[-1][1]<=4096:o[-1]=(o[-1][0],b)
    else:o.append((a,b))
m['ranges']=[dict(address=hex(a+B),size=b-a,purpose='v46r overlay vs c770f340',expected=stock[a:b].hex(),replacement=d[a:b].hex()) for a,b in o]
m['output_sha256']=sha(d);(out/'q/changed-ranges.json').write_text(json.dumps(m,indent=1)+'\n')
info=dict(base='9be24e70',sha256=sha(d),build_id=hex(nid),previous_build_id=hex(oid),live_runs=len(rs),outside_live_pb=outside,new_v37_sha256=sha(new),
          caves={'liveplan':'0x401fd000 <- B 0x402f3264','livefbh':'0x401fd100 <- BL 0x402f3c54'})
(out/'v46.main.json').write_text(json.dumps(info,indent=1)+'\n');print(json.dumps(info))
