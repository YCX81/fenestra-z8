from pathlib import Path
import collections,hashlib,json,struct
D=Path(__file__).resolve().parent
def summarize(name):
 p=D/'recovery-failure-evidence'/name;x=json.loads((p/'result.json').read_text());cs=x['live_checkpoints'];b=(p/'lv-observer.dat').read_bytes();h=struct.unpack_from('<12Q',b)
 assert h[5]==0 and h[8]==0
 rows=[struct.unpack_from('<8Q2I',b,96+(i%65536)*72) for i in range(h[4])]
 salient=[]
 for r in rows:
  if not (r[-1]>=200 and (r[-1] not in [202,208,209,210,242] or (r[-1]==202 and r[3]))):continue
  before=next((c for c in reversed(cs) if c.get('lv_diagnostics',{}).get('host_ns',0)<r[1]),None)
  after=next((c for c in cs if c.get('lv_diagnostics',{}).get('host_ns',0)>r[1]),None)
  salient.append(dict(seq=r[0],host_ns=r[1],after_checkpoint=before['label'] if before else None,before_checkpoint=after['label'] if after else None,pc=hex(r[2]),args=list(map(hex,r[3:7])),lr=hex(r[7]),cpu=r[-2],kind=r[-1]))
 waits=[]
 for r in rows:
  if r[-1]==240:
   after=next((q for q in rows[r[0]+1:] if q[-1]==241),None)
   waits.append({'seq':r[0],'event_object':hex(r[3]),'mask':hex(r[4]),'timeout_parameter':hex(r[6]),'return_seq':after[0] if after else None,'return':after[3] if after else None})
 final=cs[-1];r=final.get('runtime',{})
 rec=dict(name=name,error=x.get('error'),trace_complete=x.get('capture_trace',{}).get('complete'),firmware_sha256=x['live_a']['executed_main_sha256'],qemu_sha256=x['qemu_sha256'],driver_sha256=hashlib.sha256((p/'run_live.py').read_bytes()).hexdigest(),observer={'head':h[4],'overwritten':h[5],'reg_errors':h[8],'counts':dict(collections.Counter(q[-1] for q in rows if q[-1]>=200))},events=salient,actual_waits=waits,before=x.get('pb_recovery_before'),after=x.get('pb_recovery_after'),history=final.get('stock_error_history'),runtime={k:r.get(k) for k in ['capture.irq_seq','stream.encoded','transaction_count','transaction_error','stream.error']},pb=final.get('pb'),model_final=final.get('a2_model_final'),inputs_unchanged=x.get('input_unchanged'),owned_qemu_stopped=x.get('owned_qemu_stopped'))
 hashes={str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(p.rglob('*')) if f.is_file() and f.name!='evidence-sha256.json'}
 (p/'evidence-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
 rec['mov_sha256']=hashes['files/LIVE/100NCZ_8/DSC_0001.MOV']
 return x,rec
off,offr=summarize('v51d-a2-wait-observer');on,onr=summarize('v51d-secondary-stop-model')
(D/'qemu-v51d-secondary-stop.json').write_text(json.dumps(onr,indent=2)+'\n')
mc=lambda x:x['command'][x['command'].index('-M')+1]
on_props=mc(on).split(',');off_props=mc(off).split(',')
assert on_props.count('z8-heif-secondary-stop-model=on')==1
on_props.remove('z8-heif-secondary-stop-model=on')
assert on_props==off_props
assert onr['firmware_sha256']==offr['firmware_sha256']
assert onr['qemu_sha256']==offr['qemu_sha256']
assert onr['error'] is None and onr['trace_complete'] is True
assert onr['runtime']['transaction_count']==1 and onr['runtime']['transaction_error']==0 and onr['runtime']['stream.error']==0
assert onr['before']['irq']<onr['after']['irq'] and onr['before']['encoded']<onr['after']['encoded']
assert onr['observer']['counts'].get(200,0)==0 and onr['observer']['counts'].get(201,0)==0
assert onr['model_final']['z8-heif-secondary-stop-state']['notifications']==1
summary={'status':'passed_in_corrected_qemu_profile','core_live_capture_rec_preview_recovery_passed':True,
 'firmware_sha256':onr['firmware_sha256'],'qemu_sha256':onr['qemu_sha256'],'driver_sha256':onr['driver_sha256'],
 'functional_profile_change':'z8-heif-secondary-stop-model=on',
 'profile_comparison_exact_except_added_flag':True,
 'observer_difference':'on lane adds six passive PC probes and four final-only QOM reads; no guest writes or deadline/input/assertion changes',
 'off':{'run':offr['name'],'error':offr['error'],'actual_failure_pc':'0x4115fbe4','classifier_status':'0x100','classifier_lr':'0x40f55b60','capture_kind':'0x40','history':'0xa2','runtime':offr['runtime'],'preview_frames':offr['pb']['frames']},
 'on':{'run':onr['name'],'exit_code':0,'trace_complete':onr['trace_complete'],'runtime':onr['runtime'],'preview_frames':onr['pb']['frames'],'recovery_before':onr['before'],'recovery_after':onr['after'],'actual_waits':onr['actual_waits'],'secondary_stop_final':onr['model_final']['z8-heif-secondary-stop-state'],'capture_error_events':0,'history_error_events':0,'mov_sha256':onr['mov_sha256'],'inputs_unchanged':onr['inputs_unchanged'],'owned_qemu_stopped':onr['owned_qemu_stopped']},
 'scope_limit':'One same-firmware comparison validates this existing model profile correction. It does not establish a unique cause for all prior failures, physical timing, hardware behavior, or full native MOV playback.',
 'validated_remote_driver':'/path/to/user/fxfix/v50-playback/validated-v51d-recovery-driver/retry-driver/run_live.py',
 'validated_local_driver':str(D/'validated-v51d-recovery-driver/retry-driver/run_live.py'),
 'report':str(D/'A2-MODEL-AB.md')}
(D/'v51d-core-regression-final.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
