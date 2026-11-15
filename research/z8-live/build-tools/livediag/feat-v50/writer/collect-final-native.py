from pathlib import Path
import json,hashlib
D=Path('/path/to/user/fxfix/v50-writer');out=[]
for n in ('cwriter-422','final-silent','final-audio','final-recorded'):
 p=D/f'run-{n}';x=D/f'run-{n}.exit'
 if not x.exists():continue
 r=json.loads((p/'result.json').read_text());s=(p/'stderr.log').read_text(errors='replace');f=D/f'native-{n}.MOV'
 row={k:r.get(k) for k in ('firmware_sha256','qemu_sha256','raw_sha256','mcu_sha256','cpu_register_redirection','ram_patches','error','owned_qemu_stopped')}
 loader=next(x for x in r['command'] if x.startswith('loader,file=') and ',addr=0x40160000,' in x)
 main_path=Path(loader.split('loader,file=',1)[1].split(',addr=',1)[0])
 row['driver_reported_firmware_sha256']=row.pop('firmware_sha256')
 row['main_loader']=loader;row['loaded_main_file_sha256']=hashlib.sha256(main_path.read_bytes()).hexdigest()
 row['firmware_hash_scope']='run_native_any wrapper reports the stock hash; actual MAIN is the QEMU loader path with separately verified file hash; candidates were immutable throughout their runs'
 row.update(name=n,driver_exit=int(x.read_text()),sample_sha256=hashlib.sha256(f.read_bytes()).hexdigest(),sample_bytes=f.stat().st_size,decode_frames=s.count('Z8_WAVE5_DEC_PICTURE'),lpcm_jobs=s.count('Z8_LPCM_JOB'),unknown_accesses=[x for x in s.splitlines() if 'Z8_UNKNOWN' in x],decoder_evidence=[x for x in s.splitlines() if 'Z8_WAVE5_DEC_SEQUENCE' in x or 'Z8_WAVE5_DEC_PICTURE' in x][:3])
 out.append(row)
print(json.dumps(out,indent=2))
