#!/usr/bin/env python3
from pathlib import Path
import subprocess,json
D=Path(__file__).resolve().parent;S=D/'work/src-fix37';B=Path('/path/to/user/v37build/v52c/work/src-fix37')
flags=['--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror'];result={}
for kind,src in [('before',B),('after',S)]:
 for f in ['runtime.c','codec/live_w5enc.c']:
  name=Path(f).stem;obj=D/f'{kind}-{name}.o';subprocess.run(['clang-21',*flags,'-I',str(src),'-I',str(src/'ui/core/include'),'-I',str(src/'codec/freestanding'),'-c',str(src/f),'-o',str(obj)],check=True)
  txt=subprocess.check_output(['llvm-size-21','-A',str(obj)],text=True);rows={l.split()[0]:int(l.split()[1])for l in txt.splitlines() if l.startswith('.')};result[f'{kind}-{name}']={'alloc_code':sum(v for k,v in rows.items() if k.startswith(('.text','.rodata','.live_extra'))),'extra_bytes':rows.get('.live_extra',0)}
for codec in [0,1]:
 obj=D/f'w5-codec{codec}.o';subprocess.run(['clang-21',*flags,f'-DLIVE_W5_HEVC={codec}','-I',str(S),'-I',str(S/'ui/core/include'),'-I',str(S/'codec/freestanding'),'-c',str(S/'codec/live_w5enc.c'),'-o',str(obj)],check=True)
 for stem,entry in [('retained','live_encoder_retained'),('release','live_encoder_release')]:
  name=f'{stem}-codec{codec}';subprocess.run(['ld.lld-21','--gc-sections','-e',entry,'-Ttext=0x40100000','--defsym=memcpy=0x401c6cb4','-o',str(D/f'{name}.elf'),str(obj)],check=True)
  subprocess.run(['llvm-objcopy-21','-O','binary',str(D/f'{name}.elf'),str(D/f'{name}.bin')],check=True)
  (D/f'{name}.nm').write_text(subprocess.check_output(['llvm-nm-21','-n',str(D/f'{name}.elf')],text=True))
for ext in ['nm','bin']:(D/f'retained.{ext}').write_bytes((D/f'retained-codec1.{ext}').read_bytes())
p=subprocess.run(['clang-21',*flags,'-I',str(S),'-I',str(S/'ui/core/include'),'-I',str(S/'codec/freestanding'),'-Xclang','-fdump-record-layouts','-fsyntax-only',str(S/'codec/live_w5enc.c')],text=True,capture_output=True,check=True)
a=p.stdout.index('         0 | struct live_w5\n');b=p.stdout.index('\n\n',a);(D/'live-w5-layout.txt').write_text(p.stdout[a:b]+'\n');(D/'build-tests.json').write_text(json.dumps(result,indent=2)+'\n');print(result)
