from pathlib import Path
import sys,json,hashlib
H=Path.home();D=H/'fxfix/v50-playback';B=H/'.local/share/nikon-z8-qemu';F=H/'fxfix/v50/q/main.main'
sys.path.insert(0,str(B/'live-audio/regress/rerun-v41/retry-driver'))
from card_export import catalog,extract
card=D/'v50-final-stock4k.card.raw';rows=catalog(card)['files'];samples=[]
for row in rows:
 if row['path'].lower().endswith('.mov'):
  out=D/Path(row['path']).name
  if not out.exists():extract(card,row['path'],out)
  samples.append({'path':row['path'],'bytes':row['bytes'],'sha256':hashlib.sha256(out.read_bytes()).hexdigest()})
s=(D/'v50-final-stock4k/stderr.log').read_text(errors='replace')
hashfile=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
obj={'name':'v50-final-stock4k','exit_code':int((D/'logs/v50-final-stock4k.exit').read_text()),'main_sha256':hashfile(F),'qemu_sha256':hashfile(B/'rdw/build/qemu-system-aarch64'),'decoded_pictures':s.count('Z8_WAVE5_DEC_PICTURE'),'unknown_mmio':[l for l in s.splitlines() if 'Z8_UNKNOWN' in l],'sequences':[l for l in s.splitlines() if 'Z8_WAVE5_DEC_SEQUENCE' in l],'samples':samples,'contact_replay_sha256':hashfile(H/'fxfix/w5dec/contacts-one.json'),'tolerate_mmio':'15431000:15432000','audio_output':'none','physical_camera_verified':False,'scope':'native stock 4K movie playback completes; Live scaler is tolerated and actual concurrent Live encoding/resource handoff is unproved'}
print(json.dumps(obj,indent=2))
