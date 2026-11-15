#!/usr/bin/env python3
"""Read a completed private F run; separate duck, SFX ownership and PCM gaps.
No guest access, mutations, model behavior or inferred acoustic verdict.
"""
from pathlib import Path
import argparse,hashlib,json,struct,subprocess
ap=argparse.ArgumentParser();ap.add_argument('run',type=Path);ap.add_argument('movie',type=Path);ap.add_argument('out',type=Path);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
r=json.loads((a.run/'result.json').read_text())
info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(a.movie)]))
audio=[s for s in info['streams']if s['codec_type']=='audio'];assert len(audio)==1 and audio[0]['channels']==2 and int(audio[0]['sample_rate'])==48000
pcm=subprocess.check_output(['ffmpeg','-v','error','-i',str(a.movie),'-map','0:a:0','-c:a','pcm_s16le','-f','s16le','-']);assert len(pcm)%4==0
samples=list(struct.iter_unpack('<hh',pcm));N=len(samples);runs=[];start=None
for i,v in enumerate(samples+[(1,1)]):
 if v==(0,0):
  if start is None:start=i
 elif start is not None:
  if i-start>=480:runs.append(dict(first=start,end=i,seconds=(i-start)/48000))
  start=None
runtime=[x['runtime']for x in r.get('live_checkpoints',[])if x.get('runtime',{}).get('stream.snapshot_first',0)>0]
clip=runtime[-1]if runtime else{};begin=clip.get('stream.snapshot_first');press=clip.get('stream.clip_press')or clip.get('capture.press_us');end=clip.get('stream.clip_end')
if begin is not None:
 for x in runs:x.update(absolute_begin_us=begin+x['first']*1e6/48000,absolute_end_us=begin+x['end']*1e6/48000)
log=next((a.run/f for f in ('audio-events.jsonl','audio-events.jsonl=on')if(a.run/f).exists()),None);transitions=[];requests=[];previous=None
if log:
 for n,line in enumerate(log.read_text().splitlines(),1):
  d=json.loads(line);live=d.get('live',{});s={k:v for k,v in live.items()if k.startswith('shutter.')}
  key=tuple(sorted(s.items()))
  if s and key!=previous:
   transitions.append(dict(line=n,pc=d.get('pc'),host_ns=d.get('host_ns'),irq_timestamp_us=live.get('capture.irq_timestamp'),shutter=s,audio_phase=live.get('audio_z8.phase'),audio_blocks=live.get('audio_z8.blocks')));previous=key
  if d.get('pc')=='40d233d0'and d.get('event')in ('43180014','4318001a','43180015','43180016'):
   payload=bytes.fromhex(d.get('payload')or'');q=dict(line=n,event=d['event'],payload_first32=payload[:32].hex(),irq_timestamp_us=live.get('capture.irq_timestamp'),shutter=s)
   if d['event']=='4318001a'and len(payload)>=16:q.update(source=hex(struct.unpack_from('<Q',payload)[0]),bytes=struct.unpack_from('<I',payload,8)[0],count=struct.unpack_from('<I',payload,12)[0])
   requests.append(q)
out=dict(prepared_stock_sha256=r.get('firmware_sha256'),executed_main_sha256=r.get('live_a',{}).get('executed_main_sha256'),sfx_resource_fixture=r.get('sfx_resource_fixture'),kernel_identity=r.get('kernel_identity'),movie_sha256=hashlib.sha256(a.movie.read_bytes()).hexdigest(),movie_bytes=a.movie.stat().st_size,streams=info['streams'],pcm_frames=N,pcm_sha256=hashlib.sha256(pcm).hexdigest(),zero_runs_over10ms=runs,clip_begin_us=begin,clip_press_us=press,clip_end_us=end,declared_duck_us=[press-50000,press+100000]if press else None,shutter_transitions=transitions,native_output_requests=requests,error=r.get('error'),qemu_sha256=r.get('qemu_sha256'),boundary='Exact saved file and existing passive observations only. IRQ timestamps bound event timing and are not precise call timestamps. Counter completed alone does not establish natural or acoustic completion; check cancel_posted and native requests separately.')
(a.out/'pcm.s16le').write_bytes(pcm);(a.out/'analysis.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({k:out[k]for k in ('movie_sha256','pcm_frames','zero_runs_over10ms','clip_begin_us','clip_press_us','clip_end_us','error')}))
