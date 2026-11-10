"""Full diffs, change manifest and byte-level copy verification."""
from pathlib import Path
import subprocess,hashlib,json
R=Path(__file__).resolve().parent.parent;O=R/'audit-v36'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={}
for old,new,label in [('src-fix32','src-fix36','src'),('pbfix/live-playback-ui-icon','pb-fix36','pb')]:
 a,b=R/old,R/new
 pa={str(p.relative_to(a)):p for p in a.rglob('*') if p.is_file()}
 pb={str(p.relative_to(b)):p for p in b.rglob('*') if p.is_file()}
 records=[]
 for rel in sorted(pa.keys()|pb.keys()):
  x,y=pa.get(rel),pb.get(rel);hx,hy=sha(x) if x else None,sha(y) if y else None
  records.append({'file':rel,'before':hx,'after':hy,'status':'unchanged' if hx==hy else 'added' if not x else 'removed' if not y else 'modified'})
 manifest[label]=records
 p=R/f'{label}-v32-to-v36.diff'
 with p.open('wb') as f:
  rc=subprocess.run(['diff','-ruN',old,new],cwd=R,stdout=f).returncode
 assert rc==1
 changes=[r['file'] for r in records if r['status']!='unchanged']
 print(label,len(records),'files;',len(changes),'changed:',', '.join(changes))
 # No new diagnostic instrumentation; old baseline identifiers are kept byte-for-byte.
 added='\n'.join(l[1:] for l in p.read_text().splitlines() if l.startswith('+') and not l.startswith('+++'))
 import re
 assert not re.search(r'\b(?:fr_\w+|recorder\w*|lamp\w*|w5_diag|diag_n)\b|W5[FL][0-9%].*BIN',added,re.I)
(O/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
refs=['relive_w5.py','w5in/chain.sh','w5in/v32_patch.py']
refs+=['../feat-rec/runs/v8b-4996/report.txt','../feat-rec/FIX17-RESULT.raw','../feat-rec/runs/v9b/v8b-source.diff']
refs+=['../feat-icon/'+n for n in ('README-icon32.md','icon32_generate.py','icon32_patch.py','live-badge.svg','icon32-assets/geometry.json')]
(O/'reference-sha256.json').write_text(json.dumps({p:sha(R/p) for p in refs},indent=2)+'\n')
