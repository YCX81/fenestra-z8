"""Collect only completed G lanes; no QMP, assertions or fixture mutations."""
from pathlib import Path
import json, hashlib, re, gzip

P=Path('/path/to/user/fxfix/v53-root/native-g')
B=P.with_name('native-f')
MAIN='c14894087d5fd03b0124c7d94f6cd521e94def58831a087738145ef91b60fcd2'
MODEL='a88a921c9759deced131e372b2c9881b1f648690cfa306b51f3814b0d7961e5d'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def lines(p,prefix):return [x for x in p.read_text(errors='replace').splitlines() if x.startswith(prefix)]

for lane,expected in [('4k',95),('real',98)]:
 run=P/('run-'+lane);exitpath=P/('run-'+lane+'.exit')
 if not exitpath.exists() or not (run/'result.json').exists():
  print(lane,'RUNNING');continue
 r=json.loads((run/'result.json').read_text())
 identity=json.loads((P/('run-'+lane+'-identity.json')).read_text())
 command=r['command'];loaded=Path(command[command.index('-kernel')+1])
 stderr=run/'stderr.log';pics=lines(stderr,'Z8_WAVE5_DEC_PICTURE ');audio=lines(stderr,'Z8_LPCM_JOB ')
 oldpics=lines(B/('run-'+lane)/'stderr.log','Z8_WAVE5_DEC_PICTURE ')
 unknown=[x for x in stderr.read_text(errors='replace').splitlines() if 'unknown' in x.lower()]
 checks=r.get('checks',[]);failed=[x for x in checks if not x.get('passed')]
 exitcode=int(exitpath.read_text())
 assertions={
  'loaded_exact_G':sha(loaded)==MAIN==r['firmware_sha256']==identity['executed_main_sha256'],
  'loaded_original_model':sha(Path(command[0]))==MODEL==r['qemu_sha256'],
  'all_driver_checks':bool(checks) and not failed,
  'no_driver_error':r.get('error') is None,
  'no_unknown_mmio':not unknown,
  'all_expected_native_pictures':len(pics)==expected,
  'picture_bytes_poc_crc_equal_F':pics==oldpics,
  'eleven_LPCM_service_jobs':len(audio)==11,
  'decoder_flush_seen':bool(lines(stderr,'Z8_WAVE5_DEC_FLUSH ')),
  'normal_launcher_exit':exitcode==0,
  'owned_QEMU_stopped':r.get('owned_qemu_stopped') is True,
  'no_cpu_redirection':not r.get('cpu_register_redirection'),
  'no_RAM_patch':not r.get('ram_patches'),
 }
 summary={'schema':'v53g.native-stock-playback/v1','passed':all(assertions.values()),
  'main_sha256':MAIN,'loaded_main':str(loaded),'model_sha256':MODEL,
  'checks':len(checks),'all_checks_passed':not failed,'failed_checks':failed,
  'error':r.get('error'),'unknown_mmio':unknown,'launcher_exit':exitcode,
  'owned_qemu_stopped':r.get('owned_qemu_stopped'),
  'native_picture_calls':len(pics),'lpcm_job_calls':len(audio),
  'picture_poc_crc_sequence':pics,'lpcm_service_sequence':audio,
  'native_decoder_final_events':lines(stderr,'Z8_WAVE5_DEC_FLUSH '),
  'final_playback_state':r.get('native_model_state',{}).get('z8-playback-state'),
  'assertions':assertions,'result_sha256':sha(run/'result.json'),'stderr_sha256':sha(stderr),
  'driver_sha256':sha(P/('run-'+lane+'.py')),
  'physical_camera_verified':False,
  'scope':'Exact G ordinary native album playback using unchanged F inputs/model/assertions and private virtual card. Original MCU contacts. Audio output none: LPCM processing only, no acoustic claim. Harness stops its own QEMU after fixed observation; no claim of a production REC-to-native bridge or hardware LiveOn handoff.'}
 if lane=='real':summary['media_fixture']=json.loads((B/'real-no-preview.json').read_text())
 (P/('run-'+lane+'-summary.json')).write_text(json.dumps(summary,indent=2)+'\n')
 for f in [run/'result.json',stderr,P/('trace-'+lane+'.jsonl')]:
  name=('run-'+lane+'-'+f.name) if f.parent==run else f.name
  (P/(name+'.gz')).write_bytes(gzip.compress(f.read_bytes(),mtime=0))
 print(json.dumps({k:v for k,v in summary.items() if k not in ['picture_poc_crc_sequence','lpcm_service_sequence','assertions']},indent=2))
