"""Full record allowlist, refusal tests, concurrent-status clearance checks."""
import json,struct
from pathlib import Path
from portrait53 import apply,edits,sha
from layout_data import BASE,TABLES,rows,mapping
P=Path(__file__).parent;b=(P/'v52g.main').read_bytes();n,es=apply(b,b)
assert n==(P/'candidate.main').read_bytes()
allowed=set()
for e in es:
 o=int(e['address'],16)-BASE;allowed.update(range(o+32,o+40))
assert all(x==y or i in allowed for i,(x,y) in enumerate(zip(b,n)))
assert all(n[a-BASE+68*j:a-BASE+68*j+32]==b[a-BASE+68*j:a-BASE+68*j+32] and
           n[a-BASE+68*j+40:a-BASE+68*j+68]==b[a-BASE+68*j+40:a-BASE+68*j+68]
           for a,count in TABLES for j in range(count))
photo_preserved=0
for table in range(4):
    t=rows(b,table)
    for m in mapping(b):
        r=t.get(m['photo'][table])
        if r and any(r['P']):
            o=r['address']-BASE;assert n[o:o+68]==b[o:o+68];photo_preserved+=1
for e in es:
 bad=bytearray(b);bad[int(e['address'],16)-BASE+32]^=1
 try:apply(bytes(bad),b)
 except AssertionError:pass
 else:raise AssertionError('expected-byte rejection failed')
# Independent runtime-linked bytes survive this overlay, with exact reverse.
other=bytearray(b);other[0x100]^=1;out,_=apply(bytes(other),b);assert out[0x100]==other[0x100]
def boxes(table,group,condition=lambda row:True):
 t=rows(n,table)
 rs=[t[m['video'][table]]['P'] for m in mapping(n) if m['group']==group and condition(m['index']) and m['video'][table] in t]
 return(min(r[0]for r in rs),min(r[1]for r in rs),max(r[2]for r in rs),max(r[3]for r in rs))
clearances=[]
for t in (2,3):
 rec=boxes(t,32);timer=boxes(t,66,lambda j:j==2);res=boxes(t,34)
 assert timer[0]-rec[2]==8 and res[0]>timer[2]
 audio=boxes(t,35,lambda j:j<34);card=boxes(t,38);remaining=boxes(t,33);stock=boxes(t,19)
 gaps=[card[0]-audio[2],remaining[0]-card[2],stock[0]-remaining[2]]
 assert min(gaps)>=4,(t,gaps)
 clearances.append(dict(table=t,REC_to_elapsed=8,elapsed_to_resolution=res[0]-timer[2],audio_card_remaining_stock_gaps=gaps))
report=dict(pass_=True,sha256=sha(n),records=len(es),changed_bytes=sum(x!=y for x,y in zip(b,n)),
 photo_record_mappings_unchanged=photo_preserved,
 all_landscape_styles_and_resources_unchanged=True,refusal_cases=len(es),independent_edit_preserved=True,
 clearances=clearances,scope='Native table boxes, including conservative union of compound/alternate status rows; does not assume unrelated status is hidden.')
(P/'integrity.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
