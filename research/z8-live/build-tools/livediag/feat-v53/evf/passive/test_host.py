from pathlib import Path
import json,struct,hashlib
from decode import decode
from profile_patch import patch,SITE,LOAD,OLD
R=Path(__file__).resolve().parent
b=(R/'arm-LLG2.BIN').read_bytes();v=decode(b)
assert len(v['samples'])==4 and all(s['complete'] for s in v['samples'])
assert [s['output'] for s in v['samples']]==[4,4,4,1]
assert all(s['descriptor']['width']==720 and s['descriptor']['height']==480 for s in v['samples'])
(R/'arm-LLG2.json').write_text(json.dumps(v,indent=2)+'\n')
# If records have rolled out, report incomplete rather than filling invented values.
x=bytearray(b)
for off in [32,32+32*24]:
 for i in range((len(b)-off)//24 if off==32+32*24 else 32):
  if struct.unpack_from('<I',x,off+i*24+8)[0]==65:struct.pack_into('<I',x,off+i*24+8,0)
z=decode(x);assert z['samples'] and all(not s['complete'] and s['missing_events']==[65] for s in z['samples'])
old={}
for name in ['LIVELOG.BIN','LIVEPREV.BIN']:
 p=R.parents[1]/'card-v52/LIVEDATA'/name
 y=decode(p.read_bytes());assert y['samples']==[];old[name]=y
(R/'existing-card-logs.json').write_text(json.dumps(old,indent=2)+'\n')
# Strict one-pointer patch, all surrounding bytes retained; wrong old bytes/address rejected.
a=bytearray((SITE-LOAD)+32);struct.pack_into('<Q',a,SITE-LOAD,OLD)
t=json.loads((R/'symbols.json').read_text())['live_ui_evf_submit'];p=patch(a,t)
assert p[:SITE-LOAD]==a[:SITE-LOAD] and p[SITE-LOAD+8:]==a[SITE-LOAD+8:]
assert struct.unpack_from('<Q',p,SITE-LOAD)[0]==t
rejected=0
for data,target in [(p,t),(a,t+1),(a,0x15400000),(a,0x401fd000)]:
 try:patch(data,target)
 except AssertionError:rejected+=1
assert rejected==4
res=dict(passed=True,checks=['host decodes actual ARM ring/crumb bytes into 4 snapshots','ring/crumb duplicate records consistent','rolled-out event reports incomplete','both actual G card logs decode unchanged with no EVF diagnostics','strict pointer replacement preserves all other bytes','4 bad pointer/profile cases reject'],count=6,profile_site=hex(SITE),profile_old=hex(OLD),profile_target=hex(t),arm_fixture_sha256=hashlib.sha256(b).hexdigest())
(R/'host-result.json').write_text(json.dumps(res,indent=2)+'\n');print(json.dumps(res,indent=2))
