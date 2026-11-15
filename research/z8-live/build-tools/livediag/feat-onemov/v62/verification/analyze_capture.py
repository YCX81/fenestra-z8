#!/usr/bin/env python3
"""Analyze actual virtual-card media and passive TX/RX evidence, read-only."""
import argparse,hashlib,json,re,struct,subprocess,zlib
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--run',type=Path,required=True)
ap.add_argument('--main',type=Path,required=True)
ap.add_argument('--sound',choices=['on','off'],required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--expected-tones',type=int,nargs='+',default=[10])
ap.add_argument('--assets',type=Path,default=Path(__file__).with_name('native-sfx-assets.json'))
a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
sha=lambda b:hashlib.sha256(b).hexdigest()
main=a.main.read_bytes();assets={r['tone']:r for r in json.loads(a.assets.read_text())['resources']}
expected=[]
for t in a.expected_tones:
 r=assets[t];start=int(r['source'],16)-0x40160000;sample=main[start:start+r['bytes']]
 assert sha(sample)==r['sha256'] and r['official_input_equal']
 expected.append(sample)
tone=b''.join(expected)
result=json.loads((a.run/'result.json').read_text())
checkpoints=[json.loads(x) for x in (a.run/'live-checkpoints.jsonl').read_text().splitlines()]
final=checkpoints[-1];runtime=final.get('runtime',{})
txpath=a.run/'actual-tx.pcm';tx=txpath.read_bytes() if txpath.exists() else b''
eventpath=next(p for p in [a.run/'audio-events.jsonl',a.run/'audio-events.jsonl=on'] if p.exists())
events=[json.loads(x) for x in eventpath.read_text().splitlines()]
log=(a.run/'stderr.log').read_text();jobs=[]
for n,line in enumerate(log.splitlines(),1):
 if 'Z8_PRIVATE_TX16 ' in line:
  job=dict(re.findall(r'(\w+)=([\w]+)',line));job['line']=n;jobs.append(job)
starts=[]
for i,e in enumerate(events):
 if e.get('event')=='4318001a' and e.get('payload'):
  data=bytes.fromhex(e['payload']);source,n,count=struct.unpack_from('<QII',data)
  matching=[t for t in a.expected_tones if source==int(assets[t]['destination'],16) and n==assets[t]['bytes'] and count==1]
  if matching:starts.append((i,matching[0]))
order=['4318001a','43180015','42040110','4318001b','43180016','4318001c','42040104']
sequences=[]
for start,t in starts:
 at=0;matched=[]
 for i,e in enumerate(events[start:],start):
  if e.get('event')==order[at]:matched.append(i+1);at+=1
  if at==len(order):break
 sequences.append({'tone':t,'start_line':start+1,'ordered_event_lines':matched,'complete':at==len(order)})
movies=list((a.run/'files/LIVE').rglob('*.MOV'))
media=[]
for movie in movies:
 info=json.loads(subprocess.check_output(['ffprobe','-v','error','-count_frames','-show_streams','-of','json',str(movie)]))
 video=[s for s in info['streams'] if s['codec_type']=='video'];audio=[s for s in info['streams'] if s['codec_type']=='audio']
 pcm=subprocess.check_output(['ffmpeg','-v','error','-i',str(movie),'-map','0:a:0','-c:a','pcm_s16le','-f','s16le','-']) if audio else b''
 dec=subprocess.run(['ffmpeg','-v','error','-i',str(movie),'-f','null','-'],capture_output=True,text=True)
 n=int(video[0].get('nb_frames',0)) if len(video)==1 else 0
 zeros=[];first=None
 for i in range(len(pcm)//4+1):
  zero=pcm[i*4:i*4+4]==bytes(4) if i<len(pcm)//4 else False
  if zero and first is None:first=i
  elif not zero and first is not None:
   if i-first>=480:zeros.append({'first':first,'end':i,'seconds':(i-first)/48000})
   first=None
 row={'path':str(movie),'sha256':sha(movie.read_bytes()),'streams':info['streams'],'video_frames':n,
      'pcm_bytes':len(pcm),'pcm_sha256':sha(pcm),'pcm_nonzero':any(pcm),'zero_runs_over10ms':zeros,
      'decode_exit':dec.returncode,'decode_error':dec.stderr,
      'checks':{'one_video':len(video)==1,'one_stereo_48k_audio':len(audio)==1 and audio[0]['channels']==2 and audio[0]['sample_rate']=='48000',
                'full_decode_clean':dec.returncode==0 and not dec.stderr.strip(),'nonempty_audio':len(pcm)>48000*4 and any(pcm),
                'frame_count_guard':n>3 and n%3!=1}}
 (a.out/(movie.stem+'.s16le')).write_bytes(pcm);media.append(row)
maximum=lambda key:max((e.get('live',{}).get(key,0) for e in events),default=0)
configs=[]
for p in (a.run/'files').rglob('*'):
 if p.is_file() and p.name.upper() in ['LIVE-Z8.0','LIVE-Z8.1']:
  b=p.read_bytes();valid=(len(b)==64 and struct.unpack_from('<4I',b)==(0x31504c46,1,0x00080311,64) and zlib.crc32(b[:60])==struct.unpack_from('<I',b,60)[0])
  if valid:configs.append({'path':str(p),'generation':struct.unpack_from('<Q',b,16)[0],'requested':struct.unpack_from('<I',b,24)[0],'mute':struct.unpack_from('<I',b,28)[0],'sha256':sha(b),'crc_valid':True})
latest_config=max(configs,key=lambda c:c['generation']) if configs else {}
recovered=[]
for cp in checkpoints:
 r=cp.get('runtime',{});na=cp.get('native_audio',{})
 if (r.get('transaction_count',0)>=1 and r.get('transaction_error')==0 and
     r.get('audio.active')==1 and r.get('audio_warm')==0 and na.get('driver')==5 and
     r.get('capture.phase')==2 and r.get('stream.error')==0 and r.get('stream.job')==0 and
     r.get('shutter.completed',0)>=(len(a.expected_tones) if a.sound=='on' else 0)):
  recovered.append({'label':cp['label'],'encoded':r.get('stream.encoded'),'completed':r.get('shutter.completed'),
                    'virtual_clock':cp.get('runtime-clock-state'),'native_audio':na})
checks={'executed_main_exact':result.get('live_a',{}).get('executed_main_sha256')==sha(main),
        'no_synthetic_tone_resource':not result.get('sfx_resource_fixture'),
        'runner_no_error':result.get('error') is None,
        'native_assertions_all_passed':bool(result.get('checks')) and all(c['passed'] for c in result['checks']),
        'sound_preference':final.get('native_audio',{}).get('live_mute')==int(a.sound=='off'),
        'sound_preference_saved_crc':latest_config.get('requested')==1 and latest_config.get('mute')==int(a.sound=='off'),
        'live_saved':runtime.get('transaction_count',0)>=1 and runtime.get('transaction_error')==0,
        'microphone_recovered_after_save':bool(recovered),
        'continued_encoding_after_recovery':len(recovered)>=2 and recovered[-1]['encoded']>recovered[0]['encoded'],
        'no_shutter_errors':maximum('shutter.errors')==0,'no_cancel_timeout':maximum('shutter.cancel_posted')==0,
        'movie_media_pass':len(media)==1 and all(all(x['checks'].values()) for x in media)}
if a.sound=='on':checks.update(actual_native_pcm_byte_identical=tx==tone,
 expected_sfx_jobs=len(jobs)==len(a.expected_tones) and all(j.get('kind')=='sfx' and int(j.get('bytes',0))==assets[t]['bytes'] for j,t in zip(jobs,a.expected_tones)),
 native_completion_and_stop=[s['tone'] for s in sequences]==a.expected_tones and all(s['complete'] for s in sequences))
else:checks.update(no_sfx_pcm=not tx,no_sfx_jobs=not jobs,no_native_sfx_start=not starts)
report={'passed':all(checks.values()),'checks':checks,'run':str(a.run),'sound':a.sound,
        'main_sha256':sha(main),'qemu_sha256':result.get('qemu_sha256'),'tx_bytes':len(tx),'tx_sha256':sha(tx),
        'expected_tones':a.expected_tones,'expected_pcm_sha256':sha(tone),'jobs':jobs,'native_sequences':sequences,'media':media,
        'final_runtime':runtime,'final_native_audio':final.get('native_audio'),
        'recovered_checkpoints':recovered,
        'saved_config':configs,
        'end_state_note':'The original bounded capture driver idles 12+30+15 seconds after FULL. Recovery is required after completed SFX and successful save, with subsequent encoding progress. The final checkpoint can be after native standby; it is not mislabeled Ready.',
        'failed_native_checks':[c for c in result.get('checks',[]) if not c['passed']],
        'boundary':'Exact virtual-card files, actual DMA source consumption, native messages and real modeled RX data. No physical speaker/DAC or hardware timing claim. Capture only; native playback is separately reported.'}
(a.out/'capture-review.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'passed':report['passed'],'checks':checks,'out':str(a.out)}))
