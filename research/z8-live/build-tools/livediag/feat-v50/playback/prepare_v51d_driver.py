from pathlib import Path
import shutil,re,subprocess,struct,json,hashlib
H=Path.home();D=H/'fxfix/v50-playback/recovery-v51d-driver';S=H/'v37build/v51d/work/src-fix37';E=H/'v37build/v51d/live/payload.elf'
if not D.exists():shutil.copytree(H/'fxfix/v50-playback/recovery-error-driver',D)
fields=re.findall(r'FIELD\(([^)]+)\);',(S/'offsets.c').read_text())
assert len(fields)==len(set(fields)) and 'stream.encoded' in fields
fields.append('capture.irq_timestamp')
c='#include <stdint.h>\n#include <stddef.h>\n#include "runtime.h"\nconst uint32_t layout[]={'+','.join('offsetof(struct live_runtime,'+f+')' for f in fields)+',sizeof(struct live_runtime)};\n'
(D/'runtime-layout.c').write_text(c)
cmd=['clang-21','--target=aarch64-none-elf','-Os','-ffreestanding','-mgeneral-regs-only','-mstrict-align','-DLA_WIDTH=1024','-DLA_HEIGHT=680','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000','-DLIVE_BITRATE=8000000','-DLIVE_IDR_US=500000','-DLIVE_ENCODE_SPEED=9','-I',str(S),'-I',str(S/'ui/core/include'),'-I',str(S/'codec/freestanding'),'-c',str(D/'runtime-layout.c'),'-o',str(D/'runtime-layout.o')]
subprocess.run(cmd,check=True)
subprocess.run(['llvm-objcopy','-O','binary','--only-section=.rodata',str(D/'runtime-layout.o'),str(D/'runtime-layout.bin')],check=True)
b=(D/'runtime-layout.bin').read_bytes();assert len(b)==4*(len(fields)+1)
vals=struct.unpack('<'+'I'*(len(fields)+1),b);offsets=dict(zip(fields+['struct_bytes'],vals));assert offsets['struct_bytes']==23184
(D/'runtime-offsets.json').write_text(json.dumps(offsets,indent=2)+'\n')
p=D/'retry-driver/run_live.py';s=p.read_text();needle="    s['runtime']=runtime"
assert s.count(needle)==1;s=s.replace(needle,"    assert runtime['size']==OFFSETS['struct_bytes'], 'Runtime ABI size disagrees with v51d source layout'\n"+needle);p.write_text(s)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
r=dict(build=str(E.parent.parent),elf_sha256=sha(E),runtime_source_sha256=sha(S/'runtime.h'),layout_source_sha256=sha(D/'runtime-layout.c'),layout_object_sha256=sha(D/'runtime-layout.o'),layout_json_sha256=sha(D/'runtime-offsets.json'),driver_sha256=sha(p),commands=cmd,offsets=offsets)
(D/'layout-provenance.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({k:r[k] for k in ['elf_sha256','layout_json_sha256','driver_sha256']},indent=2));print('struct_bytes',offsets['struct_bytes'])
