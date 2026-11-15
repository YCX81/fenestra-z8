#!/usr/bin/env python3
from pathlib import Path
import json,sys,hashlib
B=Path.home()/'.local/share/nikon-z8-qemu/live-audio/regress/runs'
summary=[]
for name in sys.argv[1:]:
 D=B/name;p=D/'result.json';item={'name':name,'complete':p.exists()}
 if p.exists():
  x=json.loads(p.read_text());cs=x.get('live_checkpoints',[])
  item.update(error=x.get('error'),owned_qemu_stopped=x.get('owned_qemu_stopped'),input_unchanged=x.get('input_unchanged'),live_a=x.get('live_a'),check_failures=[q for q in x.get('checks',[]) if not q.get('passed')],checkpoint_count=len(cs))
  item['rec_owned_expectation']=x.get('rec_owned_expectation')
  item['recovery_before']=x.get('pb_recovery_before')
  item['recovery_after']=x.get('pb_recovery_after')
  item['trace_complete']=x.get('capture_trace',{}).get('complete')
  item['maxima']={k:max((c.get('runtime',{}).get(k,0) or 0 for c in cs),default=0) for k in ['full_presses','writer_returns','capture.accepted_presses','stream.encoded','transaction_count','transaction_error']}
  item['preview_max_frames']=max((c.get('pb',{}).get('frames',0) for c in cs),default=0)
  item['preview_max_lookup_error']=max((c.get('pb',{}).get('lookup_error',0) for c in cs),default=0)
  item['last_preview']=next((c['pb'] for c in reversed(cs) if 'pb' in c),None)
  item['files']=[{'path':str(f.relative_to(D/'files')),'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in sorted((D/'files').rglob('*')) if f.is_file()]
 summary.append(item)
print(json.dumps(summary,indent=2))
