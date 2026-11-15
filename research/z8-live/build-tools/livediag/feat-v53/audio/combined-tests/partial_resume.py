from pathlib import Path
import json,hashlib,subprocess
p=Path(__file__).resolve().parent;prod=p.parents[1]/'portrait/shutter/production';src=prod/'work';m=json.loads((prod/'changed-files.json').read_text())
for row in m['files']:assert hashlib.sha256((src/row['path'].removeprefix('src-fix37/')).read_bytes()).hexdigest()==row['new_sha256']
s=(src/'audio/live_shutter_z8.c').read_text().replace('section(".live_shutter"),','')
s=s.replace('#define FN(a,t) ((t)(uintptr_t)(a))','#define FN(a,t) ((t)test_function((uintptr_t)(a)))')
s=s.replace('return *(volatile uint32_t *)a;','return test_word(a);').replace('return *(volatile uintptr_t *)a;','return test_ptr(a);')
for address in ['0xa03bc7008','0xa038fe9f1']:
 s=s.replace('*(volatile uint8_t *)(uintptr_t)UINT64_C('+address+')','test_byte(UINT64_C('+address+'))')
s='#include <stdint.h>\nextern void *test_function(uintptr_t);\nextern uint32_t test_word(uintptr_t);\nextern uintptr_t test_ptr(uintptr_t);\nextern uint8_t test_byte(uintptr_t);\n'+s
(p/'shutter-host.c').write_text(s)
b=(src/'audio/live_audio_z8.c').read_text().replace('section(".live_extra"),','').replace('section(".live_shutter"),','');(p/'backend.c').write_text(b)
cmd=['clang','-O1','-g','-fsanitize=address,undefined','-I'+str(src),'-I'+str(src/'audio'),'-I'+str(p),str(p/'test_partial_resume.c'),str(p/'backend.c'),str(p/'shutter-host.c'),str(src/'audio/live_audio.c'),'-o',str(p/'test_partial_resume')]
subprocess.run(cmd,check=True);r=subprocess.run([str(p/'test_partial_resume')],capture_output=True,text=True);print(r.stdout,r.stderr);assert not r.returncode
(p/'partial-resume-result.json').write_text(json.dumps(dict(passed=True,patch=m['patch_sha256'],files=m['files'],stdout=r.stdout,scope='Actual adapter+backend+core control flow under ASan/UBSan; only native function/MMIO boundaries substituted. New held publication injected at second native GO setup post failure; not a full OS scheduling proof. Original G SFX heap/callback shape is separately covered by native ARM tests.'),indent=2)+'\n')
