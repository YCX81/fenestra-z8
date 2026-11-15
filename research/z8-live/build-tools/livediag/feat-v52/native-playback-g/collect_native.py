from pathlib import Path
import json,hashlib,shutil,tarfile,sys
version=sys.argv[1];expected=sys.argv[2]
p=Path('/path/to/user/fxfix')/(version+'-native');r=p/'run-reference'
j=json.loads((r/'result.json').read_text());ident=json.loads((p/'run-reference-identity.json').read_text());t=(r/'stderr.log').read_text();bad=[x for x in j['checks'] if not x['passed']];cmd=j['command'];loaded=Path(cmd[cmd.index('-kernel')+1]);sha=hashlib.sha256(loaded.read_bytes()).hexdigest();assert sha==j['firmware_sha256']==ident['executed_main_sha256']==expected;rc=int((p/'run-reference.exit').read_text());dec=t.count('DEC_PICTURE');lpcm=t.count('LPCM_JOB')
passed=rc==0 and not bad and not j['error'] and dec==95 and lpcm==11 and j['owned_qemu_stopped']
s=dict(result='PASS' if passed else 'FAIL',exit=rc,error=j['error'],executed_main=str(loaded),executed_main_sha256=sha,reported_main_sha256=j['firmware_sha256'],qemu_sha256=j['qemu_sha256'],mcu_sha256=j['mcu_sha256'],reference_movie_sha256='2aa43b037738eba33005aa524e69ca35b8234bbffe01ebb16923a12843dd843a',decode_frames=dec,lpcm_commands=lpcm,failed_checks=bad,checks_count=len(j['checks']),owned_qemu_stopped=j['owned_qemu_stopped'],scope=version+' Live Off native stock player; host libavcodec backend, not vendor VPU microcode execution; not Live On handoff hardware acceptance.',physical_hardware_verified=False)
(p/'summary.json').write_text(json.dumps(s,indent=2)+'\n');e=p/'evidence';e.mkdir(exist_ok=True)
for f in [p/'summary.json',p/'linked-review.json',p/'run-reference-identity.json',p/'prepared.json',p/'launch.sh',p/'run_native_v52.py',p/'run-reference.exit',r/'result.json',r/'stderr.log']:shutil.copy2(f,e/f.name)
imgs=sorted(r.glob('*.png'))
for f in imgs[-2:]:shutil.copy2(f,e/f.name)
with tarfile.open(p/'evidence.tar.gz','w:gz') as a:a.add(e,arcname='evidence')
print(json.dumps(s,indent=2));print('archive', (p/'evidence.tar.gz').stat().st_size)
