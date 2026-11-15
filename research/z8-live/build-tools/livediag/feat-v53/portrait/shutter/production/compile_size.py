from pathlib import Path
import subprocess,json,struct
from elftools.elf.elffile import ELFFile
P=Path(__file__).resolve().parent;W=P/'work';B=P.parents[3]/'feat-v52/integration-g/work/src-fix37'
names=['runtime.c','audio/live_audio.c','audio/live_audio_z8.c','audio/live_shutter_z8.c']
fields=['sizeof(struct live_runtime)','__builtin_offsetof(struct live_runtime,w5_close_lock)','__builtin_offsetof(struct live_runtime,audio_z8)','sizeof(struct live_audio_z8)','sizeof(struct live_shutter_z8)']
f=P/'abi.c';f.write_text('#include "runtime.h"\nconst unsigned long abi[]={'+','.join(fields)+'};\n')
subprocess.run(['clang','--target=aarch64-none-elf','-c','-I'+str(W),str(f),'-o',str(P/'abi.o')],check=True)
with (P/'abi.o').open('rb') as f:abi=struct.unpack('<5Q',ELFFile(f).get_section_by_name('.rodata').data())
f=W/'codec/live_w5enc.c';s=f.read_text().replace('== 23264, "v32 runtime ABI"',f'== {abi[0]}, "v53 shutter runtime ABI"').replace('== 18988, "v32 log_pad slot"',f'== {abi[1]}, "v53 shutter log_pad slot"');f.write_text(s)
report={'abi':dict(zip(fields,abi)),'source':{}}
for variant,src in [('base',B),('candidate',W)]:
 report['source'][variant]={}
 for n in names+(['codec/live_w5enc.c']if variant=='candidate'else[]):
  if not (src/n).exists():continue
  out=P/(variant+'-'+n.replace('/','_')+'.o')
  cmd=['clang','--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-mgeneral-regs-only','-mstrict-align','-DLA_WIDTH=1024','-DLA_HEIGHT=682','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000','-Wall','-Wextra','-Werror','-I'+str(src),'-I'+str(src/'ui/core/include'),'-I'+str(src/'codec/freestanding'),'-c',str(src/n),'-o',str(out)]
  subprocess.run(cmd,check=True)
  with out.open('rb') as f:
   e=ELFFile(f);secs={s.name:s['sh_size']for s in e.iter_sections()if s['sh_flags']&2}
   report['source'][variant][n]={'primary':sum(v for k,v in secs.items()if k not in ('.live_extra','.live_shutter')),'extra':secs.get('.live_extra',0),'shutter':secs.get('.live_shutter',0),'sections':secs}
report['delta']={kind:sum(d[kind]for n,d in report['source']['candidate'].items()if n!='codec/live_w5enc.c')-sum(d[kind]for d in report['source']['base'].values())for kind in ['primary','extra','shutter']}
(P/'size.json').write_text(json.dumps(report,indent=2)+'\n');print(report['abi']);print(report['delta'])
