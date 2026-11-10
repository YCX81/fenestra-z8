import datetime,hashlib,json,shutil
from pathlib import Path
D=Path(__file__).resolve().parent;H=Path('/path/to/user/.local/share/nikon-z8-qemu');R=H/'live-a-driver-20261104'
rows=json.loads((D/'batch-v4-results.json').read_text())+json.loads((D/'batch-v4-retry-results.json').read_text())
out={'all_batch_processes_complete':all('exit_code' in x for x in rows),'runs':[],'c_free_bytes':shutil.disk_usage('/mnt/c').free,'qemu_sha256':hashlib.sha256((H/'live-a-v2-qemu/build/qemu-system-aarch64').read_bytes()).hexdigest()}
for b in rows:
 n=b['name'];a=json.loads((D/(n+'-analysis.json')).read_text());p=R/n;status=a['last_status'];trace=json.loads((p/'capture-trace.json').read_text())
 presses=[e for e in trace['events'] if e['kind']=='key_consumer' and e.get('label') in ('full-press','second-full-press')]
 checkpoints=[json.loads(x) for x in (p/'live-checkpoints.jsonl').read_text().splitlines()]
 missing=[str(x) for x in p.rglob('*') if x.is_file() and x.suffix.lower() in ('.raw','.bin') and x.stat().st_size>1024**2]
 assert not missing
 entry={'name':n,'exit_code':b['exit_code'],'native_jpgs':len(a['native_jpegs']),'live_clips':len(a['media']),'full_presses':status['full_presses'],'writer_returns':status['writer_returns'],'captured':status['capture.copied'],'encoded':status['stream.encoded'],'dropped':status['capture.dropped'],'backlog_dropped':status['stream.backlog_dropped'],'copy_dropped':status['stream.copy_dropped'],'encode_avg_us':status['stream.encode_avg_us'],'encode_max_us':status['stream.encode_max_us'],'effective_fps_milli':status['stream.effective_fps_milli'],'low_fps':status['stream.low_fps'],'ring_peak':status['stream.ring_peak'],'peak_allocation':max(x.get('runtime',{}).get('capture.actual_bytes',0) for x in checkpoints),'mux_extra_encoded':status['stream.mux_end_encoded']-status['stream.mux_start_encoded'],'ignored_live_presses':status['stream.ignored_presses'],'stream_first_pts':status.get('stream.first_pts'),'press_pts':status['stream.clip_press'],'press_spacing_qemu_ns': presses[1]['t_ns']-presses[0]['t_ns'] if len(presses)==2 else None,'large_raw_bin_remaining':missing}
 out['runs'].append(entry)
for batch in ['batch-v4-results.json','batch-v4-retry-results.json']:
 bs=json.loads((D/batch).read_text());spans=[(datetime.datetime.fromisoformat(z['started_utc'])-datetime.datetime.fromisoformat(x['started_utc'])).total_seconds() for x,z in zip(bs,bs[1:])];assert all(s>=90 for s in spans);out[batch+'_start_spacing_seconds']=spans
assert out['all_batch_processes_complete']
(D/'evidence-summary.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
