from pathlib import Path
import json
D=Path.home()/'fxfix/v50-playback';R=Path.home()/'.local/share/nikon-z8-qemu/live-audio/regress/runs'
for n in ['v46-pb','v46r-pb','v50-final-pb']:
 p=D/'logs'/(n+'.log');cs=[json.loads(l[11:]) for l in p.read_text().splitlines() if l.startswith('CHECKPOINT ')]
 if not cs:continue
 x=cs[-1];r=x.get('runtime',{})
 print(n,x['label'],{k:r.get(k) for k in ['full_presses','writer_returns','transaction_count','transaction_error','stream.encoded','stream.job']},x.get('pb'))
 print('completed',(R/({'v46-pb':'v50cmp-v46-pb','v46r-pb':'v50cmp-v46r-pb'}.get(n,n))/'result.json').exists())
p=D/'v50-final-stock4k/stderr.log'
if p.exists():
 s=p.read_text(errors='replace');print('stock decoded',s.count('Z8_WAVE5_DEC_PICTURE'),'unknown',[l[:180] for l in s.splitlines() if 'Z8_UNKNOWN' in l][:5])
for p in (D/'logs').glob('*.exit'):print(p.name,p.read_text())
