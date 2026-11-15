"""Archive exact identities and sparse observations, retaining failed prototypes."""
from pathlib import Path
import hashlib,json,tarfile
ROOT=Path('/path/to/user/fxfix/v53-evf')
summaries=[]
paths=[]
for name in ['run-eye-g']+[f'run-eye{i}-g' for i in range(2,9)]:
    p=ROOT/name
    if not (p/'result.json').exists():continue
    j=json.loads((p/'result.json').read_text())
    rows=[json.loads(x) for x in (p/'eye-ui.jsonl').read_text().splitlines()]
    points=[{k:r.get(k) for k in ['vt','eye','view','view_id_state','output','output_active','root','children','descriptor','ui','observation_error']} for r in rows]
    summaries.append(dict(name=name,firmware_sha256=j.get('firmware_sha256'),qemu_sha256=j.get('qemu_sha256'),
        error=j.get('error'),owned_qemu_stopped=j.get('owned_qemu_stopped'),
        inputs=j.get('eye_inputs',[]),points=points,
        checks_total=len(j.get('checks',[])),checks_failed=[x for x in j.get('checks',[]) if not x.get('passed')],firmware_pass=False,physical_panel_verified=False))
    for f in ['result.json','eye-ui.jsonl','stderr.log']:
        paths.append(p/f)
    paths.extend(p.glob('ui-*.bin'))
    for pattern in ['output-*.bin','rgba-*.png','evf-first-spi16-*.bin']:
        paths.extend(p.glob(pattern))
    for f in [ROOT/(name+'-identity.json'),ROOT/(name+'.log'),ROOT/(name+'.exit')]:
        if f.exists():paths.append(f)
for d in ['model','model2','model3','model4']:
    for f in ['manifest.json','eye-model.patch','compile-command.json','link-command.json']:
        p=ROOT/d/f
        if p.exists():paths.append(p)
for f in ['native-eye-arm.json','observer.py','ui-offsets.json','run_eye.py','run_eye2.py','run_native_observed.py','run_native_eye2.py','launch-eye.sh','launch-eye2.sh','launch-eye3.sh','launch-eye4.sh']:
    p=ROOT/f
    if p.exists():paths.append(p)
for d in ['driver7','driver8']:
    for f in ['observer.py','service_serial8.py']:
        p=ROOT/d/f
        if p.exists():paths.append(p)
for f in ['native-geometry-arm.json','test_native_geometry.py','test_native_eye.py','launch-eye5.sh','launch-eye6.sh','launch-eye7.sh','launch-eye8.sh']:
    p=ROOT/f
    if p.exists():paths.append(p)
out=ROOT/'summary.json';out.write_text(json.dumps(summaries,indent=2)+'\n');paths.append(out)
archive=ROOT/'evidence.tar.gz'
with tarfile.open(archive,'w:gz') as t:
    for p in paths:t.add(p,arcname=str(p.relative_to(ROOT)))
print(json.dumps(dict(runs=len(summaries),archive=str(archive),sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),bytes=archive.stat().st_size),indent=2))
