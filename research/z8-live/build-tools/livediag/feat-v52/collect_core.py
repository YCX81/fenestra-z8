"""Collect a completed exact-image core regression and independently decode its MOV."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('main',type=Path);p.add_argument('out',type=Path)
a=p.parse_args();d=json.loads((a.run/'result.json').read_text());sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
actual=sha(a.main);assert d['live_a']['executed_main_sha256']==actual
kernel=Path(d['command'][d['command'].index('-kernel')+1]);assert kernel.resolve()==a.main.resolve() and sha(kernel)==actual
assert d.get('error') is None and d['owned_qemu_stopped'] is True
assert d['checks'] and all(c['passed'] for c in d['checks'])
before=d['pb_recovery_before'];after=d['pb_recovery_after']
assert after['irq']>before['irq'] and after['encoded']>before['encoded'] and after['preview_frames']>0
final=next(c for c in reversed(d['live_checkpoints']) if c.get('label')=='final')
r=final['runtime'];assert r['transaction_count']==1 and r['transaction_error']==0
assert r['stream.error']==0 and r['crumb_pad']==0 and final['pb']['error']==0
files=list((a.run/'files').glob('LIVE/**/*.MOV'));assert len(files)==1;movie=files[0]
probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(movie)]))
video=next(s for s in probe['streams'] if s.get('codec_type')=='video')
assert video['codec_name']=='hevc' and video['avg_frame_rate']==video['r_frame_rate']=='30000/1001'
assert video['time_base']=='1/30000';frames=int(video['nb_frames']);duration=frames*1001/30000
assert abs(float(video['duration'])-duration)<0.000001
dec=subprocess.run(['ffmpeg','-v','error','-i',str(movie),'-map','0:v:0','-f','null','-'],capture_output=True,text=True)
assert dec.returncode==0 and not dec.stderr,dec.stderr
result=dict(passed=True,run=str(a.run),result_sha256=sha(a.run/'result.json'),
            main_sha256=actual,kernel_path=str(kernel),prepared_stock_sha256=d.get('firmware_sha256'),
            checks=len(d['checks']),before=before,after=after,transactions=r['transaction_count'],
            final_irq=r['capture.irq_seq'],final_encoded=r['stream.encoded'],final_hold=r['crumb_pad'],
            native_w5=final.get('w5_close_diag'),qemu_sha256=d['qemu_sha256'],
            movie=dict(source=str(movie),sha256=sha(movie),bytes=movie.stat().st_size,frames=frames,
                       width=video['width'],height=video['height'],rate='30000/1001',duration=duration,
                       full_ffmpeg_decode=True,audio_tracks=sum(s.get('codec_type')=='audio' for s in probe['streams'])),
            physical_camera_verified=False,scope='Actual Live capture, 45-frame REC preview, half-press return and new Live encoding; emulator microphone produces no PCM')
a.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
