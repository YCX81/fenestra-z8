from pathlib import Path
import json,hashlib,subprocess
p=Path(__file__).resolve().parent;prod=p.parents[1]/'portrait/shutter/production';src=prod/'work';m=json.loads((prod/'changed-files.json').read_text())
for row in m['files']:assert hashlib.sha256((src/row['path'].removeprefix('src-fix37/')).read_bytes()).hexdigest()==row['new_sha256']
(p/'baseline_lifecycle.c').write_bytes((prod/'baseline_lifecycle.c').read_bytes())
b=(src/'audio/live_audio_z8.c').read_text().replace('section(".live_extra"),','').replace('section(".live_shutter"),','');(p/'backend.c').write_text(b)
d=(src/'runtime.c').read_text();i=d.index('static void live_audio_ready_step(');start=d.index('{',i);depth=1;j=start+1
while depth:
 if d[j]=='{':depth+=1
 elif d[j]=='}':depth-=1
 j+=1
(p/'ready.inc').write_text(d[i:j]+'\n')
subprocess.run(['clang','-O1','-g','-fsanitize=address,undefined','-I'+str(src),'-I'+str(src/'audio'),'-I'+str(p),str(p/'test_combined.c'),str(p/'backend.c'),str(src/'audio/live_audio.c'),str(src/'codec/au_ring.c'),'-o',str(p/'test_combined')],check=True)
r=subprocess.run([str(p/'test_combined')],capture_output=True,text=True);print(r.stdout,r.stderr);assert r.returncode==0
(p/'result.json').write_text(json.dumps(dict(passed=True,patch=m['patch_sha256'],files=m['files'],stdout=r.stdout,scope='Independent actual C backend+core+verbatim runtime readiness, ASan/UBSan; native message/codec boundaries mocked. SFX adapter execution independently covered by portrait original-ARM tests.'),indent=2)+'\n')
