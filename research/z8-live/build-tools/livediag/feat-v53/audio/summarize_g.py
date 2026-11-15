"""Actual G final receipt; every acceptance flag derives from completed artifacts."""
from pathlib import Path
import json,hashlib,struct,re,sys
p=Path(sys.argv[1]);r=json.loads((p/'result.json').read_text());cp=r['live_checkpoints'][-1];rt=cp['runtime'];pb=cp['pb'];pcm=json.loads((p/'pcm-summary.json').read_text());ident=json.loads((p/'identity-and-capture-summary.json').read_text())
sha=lambda b:hashlib.sha256(b).hexdigest()
wave=(p/'mov-audio.s16le').read_bytes();ring=(p/'audio-events.jsonl=on.pcm').read_bytes();tx=(p/'actual-tx.pcm').read_bytes();main=Path(r['kernel_identity']['path']).read_bytes();assert sha(main)=='c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2'
tone=main[0x43fba590-0x40160000:0x43fba590-0x40160000+4800];cursor=0;jobs=[];rec=bytearray();sfx=bytearray()
for l in (p/'stderr.log').read_text(errors='replace').splitlines():
 m=re.search(r'Z8_PRIVATE_TX16 job=(\d+) kind=(\w+) ns=(\d+) source=([0-9a-f]+) bytes=(\d+) channel=(\d+)',l)
 if not m:continue
 job,kind,ns,source,nb,ch=m.groups();nb=int(nb);chunk=tx[cursor:cursor+nb];assert len(chunk)==nb
 jobs.append({'job':int(job),'kind':kind,'qom_ns':int(ns),'source':source,'bytes':nb,'channel':int(ch),'export_offset':cursor,'sha256':sha(chunk)});cursor+=nb
 if kind=='ring192000':rec+=chunk
 elif kind=='sfx4800':sfx+=chunk
 else:raise AssertionError(kind)
assert cursor==len(tx)
movpath=next((p/'files/LIVE').rglob('*.MOV'));mov=movpath.read_bytes();atom=len(mov)-struct.unpack('<I',mov[-8:-4])[0];nf,dt=struct.unpack_from('<II',mov,atom+20)
span=rt['stream.snapshot_last']-rt['stream.snapshot_first']+rt['stream.last_duration_us'];header_good=0<=nf*dt-span<45
complete=len(rec)>=len(wave) and rec[:len(wave)]==wave and not any(rec[len(wave):]) and ring[:len(wave)]==wave
failed=[x for x in r['checks'] if not x['passed']];ready=rt['audio_warm']==0 and rt['audio.active']==1 and rt['capture.phase']==2 and rt['stream.job']==0 and rt['stream.encoded']>r['pb_recovery_before']['encoded']
unknown=[l for l in (p/'stderr.log').read_text(errors='replace').splitlines() if 'Z8_UNKNOWN' in l]
window_ok=pcm['leading_zero_frames']==0 and pcm['trailing_zero_frames']==0 and pcm['lvjb_same_track'] and not pcm['audio_decode_errors'] and not pcm['video_decode_errors'] and all(x['seconds']<=0.2 for x in pcm['zero_runs_10ms'])
shutter_ok=rt['shutter.completed']==2 and rt['shutter.cancel_posted']==0 and rt['shutter.cancelled']==0 and rt['shutter.errors']==0 and sfx==tone*2
rec_ok=complete and pb['audio_stage']==0 and pb['audio_gate']==0 and pb['audio_reason']==0 and pb['audio_errors']==0 and pb['audio_plays']==1 and not pb['playing'] and not cp['serial5-tx-drain-state']['fault']
s={'core_runner_error':r['error'],'core_runner_checks':{'total':len(r['checks']),'failed':failed},'capture_audio_window_passed':window_ok,'shutter_valid_resource_lifecycle_passed':shutter_ok,'rec_pcm_dma_passed':rec_ok,'returned_to_audio_ready':ready,'preview_actual_span_passed':header_good,'all_required_passed':r['error'] is None and not failed and not unknown and window_ok and shutter_ok and rec_ok and ready and header_good}
s.update({'scope':'Exact G completed run with strict private counter-domain model and synthetic25ms resource4e. PCM input and output DMA/IRQ/firmware lifecycle tested; no physical ADC/DAC/acoustic/tone2-duration claim. Zero-window pass requires no leading/trailing silence and all long zeros <=200ms; independent shutter/duck timeline review accompanies this receipt.','capture_audio':pcm,'shutter':{k:rt[k] for k in rt if k.startswith('shutter.')},'rec':{'preview_frames':nf,'preview_frame_us':dt,'preview_duration_us':nf*dt,'frozen_video_span_us':span,'audio_source_frames':len(wave)//4,'audio_duration_us_ceil':(len(wave)*1000+191)//192,'source_pcm_bytes':len(wave),'source_pcm_sha256':sha(wave),'ring_bytes':len(ring),'ring_sha256':sha(ring),'ring_prefix_exact':ring[:len(wave)]==wave,'actual_rec_dma_bytes':len(rec),'complete_source_pcm_exact':complete,'zero_tail_bytes':max(0,len(rec)-len(wave)),'actual_tx_sha256':sha(tx),'rec_export_sha256':sha(rec),'pb_final':pb,'tx_final':cp['serial5-tx-drain-state'],'jobs':jobs},'identities':{'kernel':r['kernel_identity'],'model_sha256':r['qemu_sha256'],'driver_sha256':sha((p/'run_live.py').read_bytes()),'feeder_sha256':sha((p/'pcm_feeder.py').read_bytes()),'observer':r['audio_protocol_observer'],'fixture':r['sfx_resource_fixture'],'pcm_fixture_sha256':'57720e7139fb9afd6ecf7e942ac4a9752dad4a7c04f44dde203ae58e72b7ee15','clock':r['pcm_clock_coordinates'],'runtime_layout':json.loads((p/'g-abi.json').read_text())},'final_state':{k:rt[k] for k in ['audio_warm','audio.active','audio_z8.phase','capture.phase','stream.job','stream.encoded','audio_tracks','transaction_count','transaction_error']},'media':ident['media'],'external_input':{'stop_cancellations':sum(1 for l in (p/'pcm-stop-cancellations.jsonl').read_text().splitlines() if l) if (p/'pcm-stop-cancellations.jsonl').exists() else 0,'gaps':sum(1 for x in p.glob('*gap*.jsonl') for l in x.read_text().splitlines() if l)},'source_run':str(p),'unknown':unknown})
(p/'final-summary.json').write_text(json.dumps(s,indent=2)+'\n');(p/'rec-export.s16le').write_bytes(rec)
print(json.dumps({'summary_sha256':sha((p/'final-summary.json').read_bytes()),'gates':{k:v for k,v in s.items() if k.endswith('passed')},'checks':len(r['checks']),'error':r['error'],'preview_us':nf*dt,'audio_frames':len(wave)//4,'REC_export':len(rec),'zero_runs':pcm['zero_runs_10ms']}))
