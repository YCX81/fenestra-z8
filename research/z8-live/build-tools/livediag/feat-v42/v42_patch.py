"""v42 = v41 (ee11bb57) + SD DMA cache fix: cardslot cave rebuilt from feat-v42/cardslot.c with two more
stock wrappers hooked: read 0x402dd6c8 (civac before+after) and write 0x402dd78c (cvac before).
The five FENESTRA path hooks are unchanged in behaviour. Bootguard build ID restamped."""
import hashlib,json,struct,sys
from pathlib import Path
H=Path.home();B=0x40160000;BUILD_ID=0x401fe060
sys.path.insert(0,str(H/'fxfix/cardslot'))
import cardslot_patch as cp
cp.HERE=Path(__file__).resolve().parent
cp.WRAPPERS=dict(cp.WRAPPERS,read=(0x402dd6c8,'stp x25, x24, [sp, #-0x40]!','f963bca9'),write=(0x402dd78c,'sub sp, sp, #0x60','ff8301d1'))
sha=lambda b:hashlib.sha256(b).hexdigest()
d=bytearray((H/'fxfix/v41/q/main.main').read_bytes());assert sha(d).startswith('ee11bb57')
stock=(H/'fxfix/base-c770f340.main').read_bytes()
for name,(entry,_,enc) in cp.WRAPPERS.items():
    if name in ('read','write'):assert d[entry-B:entry-B+4].hex()==enc==stock[entry-B:entry-B+4].hex(),name
d[cp.CAVE-B:cp.CAVE_END-B]=bytes(cp.CAVE_END-cp.CAVE)
cave,syms,text=cp.build_cave();assert cp.CAVE+len(cave)<=cp.CAVE_END
d[cp.CAVE-B:cp.CAVE-B+len(cave)]=cave
hooks={}
for name,(entry,insn,_) in cp.WRAPPERS.items():
    br=cp.branch(entry,syms['hook_'+name]);d[entry-B:entry-B+4]=br;hooks[name]=dict(entry=hex(entry),original=insn,branch=br.hex())
old=struct.unpack_from('<I',d,BUILD_ID-B)[0];struct.pack_into('<I',d,BUILD_ID-B,0)
nid=struct.unpack('<I',hashlib.sha256(bytes(d)).digest()[:4])[0]|0x01000000;struct.pack_into('<I',d,BUILD_ID-B,nid)
d=bytes(d);out=H/'fxfix/v42';(out/'q').mkdir(parents=True,exist_ok=True)
(out/'q/main.main').write_bytes(d);(out/'q/cave.txt').write_text(text)
m=json.loads((H/'fxfix/v41/q/changed-ranges.json').read_text())
for k,v in syms.items():m['symbols']['cardslot.'+k]=v
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
m['ranges']=[dict(address=hex(a+B),size=b-a,purpose='v42 overlay vs c770f340',expected=stock[a:b].hex(),replacement=d[a:b].hex()) for a,b in o]
m['output_sha256']=sha(d);(out/'q/changed-ranges.json').write_text(json.dumps(m,indent=1)+'\n')
v41=(H/'fxfix/v41/q/main.main').read_bytes()
diff=[(hex(a+B),b-a) for a,b in runs(v41,d)]
info=dict(base='ee11bb57',sha256=sha(d),build_id=hex(nid),previous_build_id=hex(old),cave=[hex(cp.CAVE),hex(cp.CAVE+len(cave))],hooks=hooks,
          cardslot_symbols={k:hex(v) for k,v in sorted(syms.items())},diff_vs_v41=diff)
(out/'v42.main.json').write_text(json.dumps(info,indent=1)+'\n')
print(sha(d)[:16],hex(nid),len(cave),diff)
