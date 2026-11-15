from pathlib import Path
import json,hashlib,struct,re
R=Path('/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/v53g-readonly-gui2-counter-audio-capture-rec');P=Path(__file__).resolve().parent
r=json.loads((R/'result.json').read_text());a=json.loads((P/'analysis.json').read_text());pcm=(P/'pcm.s16le').read_bytes();tx=(R/'actual-tx.pcm').read_bytes();raw=[json.loads(x)for x in (R/'audio-events.jsonl=on').read_text().splitlines()]
assert a['movie_sha256']=='1b81760796b54021ea8732f9fc9aac82d264cf0c96e1b9eb6f8ee2f818d3a741'
assert a['executed_main_sha256']=='c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2'
assert len(pcm)==145745*4 and pcm[:4]!=bytes(4)and pcm[-4:]!=bytes(4)
assert tx[9600:9600+len(pcm)]==pcm and not any(tx[9600+len(pcm):])
TH='38299bea8895a8cbea19c72f6a741b5908e2d6691c7b6525f6370510c625d10a'
assert hashlib.sha256(tx[:4800]).hexdigest()==TH and hashlib.sha256(tx[4800:9600]).hexdigest()==TH
assert max(x.get('live',{}).get('shutter.cancel_posted',0)for x in raw)==0
assert max(x.get('live',{}).get('shutter.errors',0)for x in raw)==0
sfx=[]
for first,last in [(234,256),(282,304)]:
 rows=raw[first-1:last];seq=[x.get('event')for x in rows];order=['43180014','4318001a','43180015','42040110','4318001b','43180016','4318001c','42040104'];j=0
 for event in seq:
  if j<len(order)and event==order[j]:j+=1
 assert j==len(order)
 start=rows[0]['live']['shutter.began'];sfx.append(dict(first_line=first,last_line=last,ordered_events=order,began_us=start,cancel_posted=0))
stderr=(R/'stderr.log').read_text().splitlines();jobs=[]
for n,line in enumerate(stderr,1):
 if 'Z8_PRIVATE_TX16' in line and 'kind=sfx4800' in line:
  ns=int(re.search(r'ns=(\d+)',line)[1]);jobs.append(dict(line=n,text=line,virtual_ns=ns,guest_us=ns/984))
assert len(jobs)==2
for item,job in zip(sfx,jobs):item.update(tx=job,began_to_consumed_us=job['guest_us']-item['began_us']);assert 0<item['began_to_consumed_us']<100000
clip_first=a['clip_begin_us']*48000//1000000;press=a['clip_press_us']*48000//1000000
zero=a['zero_runs_over10ms'];duck_first=press-2400-clip_first;duck_end=press+4800-clip_first
assert zero[0]['first']==duck_first-1 and zero[0]['end']==duck_end
# Actual pause begins with old next index; first resumed valid block advances by its 2400 samples.
old=raw[274]['live']['audio.next_index'];new_end=raw[316]['live']['audio.next_index'];new_first=new_end-2400
assert raw[302]['live']['audio_z8.resume_bridge']==1 and raw[316]['live']['audio_z8.resume_bridge']==0
assert zero[1]['first']==old-clip_first-1 and zero[1]['end']==new_first-clip_first
assert new_first-old==4181
# First SFX gap lies inside the existing duck range and does not add a third long zero run.
one_old=raw[226]['live']['audio.next_index'];anchor1=raw[274]['live']['audio_z8.anchor_us'];one_new=anchor1*48000//1000000
assert duck_first<=one_old-clip_first<one_new-clip_first<=duck_end
final=r['live_checkpoints'][-1];pb=final['pb'];live=final['runtime'];assert r['error'] is None and all(c['passed']for c in r['checks'])
assert pb['frames']==45 and pb['last_frame']==44 and live['capture.phase']==2 and live['audio_warm']==0 and live['audio.active']==1
out=dict(passed=True,source_run=str(R),main_sha256=a['executed_main_sha256'],movie_sha256=a['movie_sha256'],pcm_frames=len(pcm)//4,pcm_sha256=hashlib.sha256(pcm).hexdigest(),pcm_no_leading_or_trailing_zeros=True,full_rec_prefix_byte_identical=True,rec_pcm_bytes=len(pcm),rec_padding_zero_bytes=len(tx)-9600-len(pcm),sfx=sfx,duck=dict(first=duck_first,end=duck_end,samples=7200,waveform_adjacent_zero_samples=1),first_sfx_bridge=dict(first=one_old-clip_first,end=one_new-clip_first,inside_duck=True),second_sfx_bridge=dict(first=old-clip_first,end=new_first-clip_first,samples=new_first-old,seconds=(new_first-old)/48000,waveform_adjacent_zero_samples=1),checks=len(r['checks']),preview_frames=pb['frames'],last_frame=pb['last_frame'],returned_true_ready=True,source_sha256={n:hashlib.sha256((R/n).read_bytes()).hexdigest()for n in ['result.json','audio-events.jsonl=on','stderr.log','actual-tx.pcm','pcm-summary.json']},boundary='Original saved files and passive events only. Natural service completion and RAM consumption are proven for the declared synthetic resource and strict functional TX model; no physical DAC/acoustic or camera timing claim. Prior G GUI failure retained separately; passing repeat does not establish its root cause.')
(P/'independent-media-review.json').write_text(json.dumps(out,indent=2)+'\n');print('PASS exactG media: two natural SFX, explicit short bridges, all145745PCM frames in REC,45pictures,realReady')
