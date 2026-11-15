python3 - <<'P'
from pathlib import Path
import json
p=Path.home()/'fxfix/v50-playback/logs/v50-final-pb.log'
cs=[json.loads(l[11:]) for l in p.read_text().splitlines() if l.startswith('CHECKPOINT ')]
for c in cs[-2:]:
 r=c.get('runtime',{});print(c['label'], {k:r.get(k) for k in ['requested','capture.phase','capture.reason','capture.irq_seq','stream.job','stream.encoded','stream.allocation_requested','stream.error']},'map',c.get('w5_client_map'),'vdi',c.get('w5_vdi'),'menu',c.get('menu',{}).get('page_id'),c.get('lv_diagnostics',{}).get('last_irq_progress_checkpoint'))
D=Path.home()/'.local/share/nikon-z8-qemu/live-audio/regress/runs/v50-final-pb'
print([p.name for p in D.iterdir() if p.suffix=='.bin' and any(q in p.name for q in ['runtime','live-state','engine','probe'])][-20:])
P
