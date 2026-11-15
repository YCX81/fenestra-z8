import json
from pathlib import Path
R=Path('/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104')
for name in ['v4-on','v4-off','v4-two','v4-retry-on','v4-retry-two']:
 p=R/name/'live-checkpoints.jsonl'
 if not p.exists():continue
 try:
  lines=p.read_text().splitlines();row=json.loads(lines[-1]);r=row.get('runtime',{})
  keys=['requested','full_presses','jpeg_returns','capture.phase','capture.copied','capture.dropped','capture.actual_bytes','stream.encoded','stream.width','stream.height','stream.encode_avg_us','stream.encode_max_us','stream.effective_fps_milli','stream.job','stream.frozen_count','stream.error','stream.geometry_changes','transaction_count','transaction_error','debug_fs_error']
  print(name,len(lines),row['label'],{k:r[k] for k in keys if k in r})
 except (json.JSONDecodeError,IndexError):print(name,'checkpoint write in progress')
 if (R/name/'files').exists():print('files',[str(x.relative_to(R/name/'files')) for x in (R/name/'files').rglob('*') if x.is_file()])
print(Path('batch-v4.log').read_text())
