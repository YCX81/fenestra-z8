"""v41 = v40 (d1e6a6df) + SD fallback core routing (cardslot.c cave) + Live/playback two-card changes.
Runs on win-lab. Usage: v41_patch.py
1. Splice the Live/playback layer diff (v37 font build 5434c24f -> v37 v41 build) onto v40; every spliced
   byte must still equal the font build in v40 (no later layer owns it), except the bootguard build ID.
2. Link cardslot.c + trampolines into the RX cave 0x401fe300.. and branch the five stock path wrappers
   into it (same code as cardslot_patch.py, which built 61d5896f).
3. Restamp the bootguard build ID; write changed-ranges.json vs c770f340 for the QEMU driver."""
import hashlib,json,struct,sys
from pathlib import Path
H=Path.home();B=0x40160000;BUILD_ID=0x401fe060
sys.path.insert(0,str(H/'fxfix/cardslot'))
import cardslot_patch as cp
sha=lambda b:hashlib.sha256(b).hexdigest()
V40='d1e6a6df5d144543f0f0da0713cac128e744546264d99023d4d86ec5c6dda522'
base=bytearray((H/'fxfix/font/q/main.main').read_bytes());assert sha(base)==V40
old=(H/'v37build/font/v37.main').read_bytes();new=(H/'v37build/v41/v37.main').read_bytes()
assert sha(old).startswith('5434c24f')
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
bid=(BUILD_ID-B,BUILD_ID-B+4);ov=lambda a,b:a[0]<b[1] and b[0]<a[1]
rs=[r for r in runs(old,new) if not ov(r,bid)]
for a,b in rs:
    assert base[a:b]==old[a:b],'later layer owns %#x'%(a+B)
    base[a:b]=new[a:b]
span=(min(a for a,_ in rs)+B,max(b for _,b in rs)+B)
outside=[(a,b) for a,b in rs if not (0x401c0000<=a+B<0x401fe000)]
Path(H/'fxfix/v41/outside.json').write_text(json.dumps([[hex(a+B),b-a,old[a-a%4:b+(-b)%4].hex(),new[a-a%4:b+(-b)%4].hex()] for a,b in outside]))
print('outside payload runs:',len(outside))
# 2. core cave
for name,(entry,_,enc) in cp.WRAPPERS.items():assert base[entry-B:entry-B+4].hex()==enc,name
assert base[cp.CLOSEDIR[0]-B:cp.CLOSEDIR[0]-B+4].hex()==cp.CLOSEDIR[1]
assert not any(base[cp.CAVE-B:cp.CAVE_END-B]),'cave not empty'
old_id=struct.unpack_from('<I',base,BUILD_ID-B)[0]
assert old_id&0x01000000 and base[0x401fe068-B:0x401fe070-B]==struct.pack('<Q',0x31544f4f425846)
cave,syms,text=cp.build_cave();assert cp.CAVE+len(cave)<=cp.CAVE_END
base[cp.CAVE-B:cp.CAVE-B+len(cave)]=cave
hooks={}
for name,(entry,insn,_) in cp.WRAPPERS.items():
    br=cp.branch(entry,syms['hook_'+name]);base[entry-B:entry-B+4]=br;hooks[name]=dict(entry=hex(entry),original=insn,branch=br.hex())
# 3. build id
struct.pack_into('<I',base,BUILD_ID-B,0)
nid=struct.unpack('<I',hashlib.sha256(bytes(base)).digest()[:4])[0]|0x01000000
struct.pack_into('<I',base,BUILD_ID-B,nid)
d=bytes(base);out=H/'fxfix/v41';(out/'q').mkdir(parents=True,exist_ok=True)
(out/'q/main.main').write_bytes(d);(out/'q/cave.txt').write_text(text)
stock=(H/'fxfix/base-c770f340.main').read_bytes()
m=json.loads((H/'fxfix/font/q/changed-ranges.json').read_text())
lm=json.loads((H/'v37build/v41/live/changed-ranges.json').read_text())['symbols']
fm=json.loads((H/'v37build/font/live/changed-ranges.json').read_text())['symbols']
syms_all=dict(m['symbols'])
for k,v in lm.items():
    if 0x401e2000<=v<0x401fe000 or (k in fm and fm[k]!=v) or k not in syms_all:syms_all[k]=v
for k,v in syms.items():syms_all['cardslot.'+k]=v
m['symbols']=syms_all
def merged(rs,gap=4096):
    o=[]
    for a,b in rs:
        if o and a-o[-1][1]<=gap:o[-1]=(o[-1][0],b)
        else:o.append((a,b))
    return o
m['ranges']=[dict(address=hex(a+B),size=b-a,purpose='v41 overlay vs c770f340',expected=stock[a:b].hex(),replacement=d[a:b].hex()) for a,b in merged(runs(stock,d))]
m['output_sha256']=sha(d)
(out/'q/changed-ranges.json').write_text(json.dumps(m,indent=1)+'\n')
info=dict(base='d1e6a6df',sha256=sha(d),build_id=hex(nid),previous_build_id=hex(old_id),live_runs=len(rs),live_span=[hex(x) for x in span],
          new_v37_sha256=sha(new),cave=[hex(cp.CAVE),hex(cp.CAVE+len(cave))],ram_state=hex(cp.RAM_STATE),hooks=hooks,
          cardslot_symbols={k:hex(v) for k,v in sorted(syms.items())})
(out/'v41.main.json').write_text(json.dumps(info,indent=1)+'\n')
print(json.dumps({k:info[k] for k in ('sha256','build_id','previous_build_id','live_runs','live_span','cave')},indent=1))
