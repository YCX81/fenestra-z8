from pathlib import Path
import ast,subprocess,json,hashlib
H=Path.home();D=H/'fxfix/v52-handoff';S=D/'work/src-fix37';B=H/'v37build/v51d';O=D/'live';O.mkdir(exist_ok=True)
names=next(ast.literal_eval(n.value) for n in ast.parse((B/'relive_v37.py').read_text()).body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='names' for t in n.targets))
flags=['--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-mgeneral-regs-only','-mstrict-align','-DLA_WIDTH=1024','-DLA_HEIGHT=680','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000','-DLIVE_BITRATE=8000000','-DLIVE_IDR_US=500000','-DLIVE_ENCODE_SPEED=9','-Wall','-Wextra','-Werror','-I',str(S),'-I',str(S/'ui/core/include'),'-I',str(S/'codec/freestanding')]
objects=[]
for name in names:
 p=O/(name.replace('/','_')+'.o');subprocess.run(['clang-21',*flags,'-c',str(S/name),'-o',str(p)],check=True);objects.append(str(p))
ld=(B/'live/payload.ld').read_text().replace('ASSERT(. <= 0x401fff80','ASSERT(. <= 0x401fd000')
(O/'payload.ld').write_text(ld)
q=subprocess.run(['ld.lld','-T',str(O/'payload.ld'),*objects,'-o',str(O/'payload.elf')],capture_output=True,text=True)
result={'compile_objects':len(objects),'warnings_as_errors':True,'strict_limit':'0x401fd000','strict_link_pass':q.returncode==0,'link_stderr':q.stderr,'audio_test_dependency':'v52 audio candidate; included only for compilation/integration test'}
if q.returncode==0:
 subprocess.run(['llvm-objcopy','-O','binary',str(O/'payload.elf'),str(O/'payload.bin')],check=True)
 b=(O/'payload.bin').read_bytes();result.update(payload_bytes=len(b),baseline_bytes=(B/'live/payload.bin').stat().st_size,growth=len(b)-(B/'live/payload.bin').stat().st_size,payload_sha256=hashlib.sha256(b).hexdigest(),remaining_bytes=0x401fd000-0x401e2000-len(b))
(D/'build-live-result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));raise SystemExit(q.returncode)
