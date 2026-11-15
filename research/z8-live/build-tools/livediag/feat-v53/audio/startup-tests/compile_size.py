from pathlib import Path
import subprocess,json,struct
from elftools.elf.elffile import ELFFile
p=Path(__file__).resolve().parent;base=p.parent/'work/src-fix37';src=p.parent/'capture-work/src-fix37'
fields=['sizeof(struct live_runtime)','__builtin_offsetof(struct live_runtime,audio)','__builtin_offsetof(struct live_runtime,audio_z8)','__builtin_offsetof(struct live_runtime,audio_retry_us)','__builtin_offsetof(struct live_runtime,flag_req)','__builtin_offsetof(struct live_runtime,crumb_head)','sizeof(struct live_audio_z8)']
result={}
for name,s in [('base',base),('candidate',src)]:
 c=p/(name+'-abi.c');c.write_text('#include "runtime.h"\nconst unsigned long abi[]={'+','.join(fields)+'};\n')
 subprocess.run(['clang','--target=aarch64-none-elf','-c','-I'+str(s),str(c),'-o',str(p/(name+'-abi.o'))],check=True)
 with (p/(name+'-abi.o')).open('rb') as f:d=ELFFile(f).get_section_by_name('.rodata').data();result[name+'_abi']=struct.unpack('<7Q',d)
 result[name+'_sizes']={}
 for n in ['runtime.c','audio/live_audio.c','audio/live_audio_z8.c']:
  out=p/(name+'-'+n.replace('/','_')+'.o')
  cmd=['clang','--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-mgeneral-regs-only','-mstrict-align','-DLA_WIDTH=1024','-DLA_HEIGHT=682','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000','-Wall','-Wextra','-Werror','-I'+str(s),'-I'+str(s/'ui/core/include'),'-I'+str(s/'codec/freestanding'),'-c',str(s/n),'-o',str(out)]
  subprocess.run(cmd,check=True)
  with out.open('rb') as f:
   e=ELFFile(f);secs={s.name:s.data_size for s in e.iter_sections() if s.name.startswith('.text') or s.name=='.live_extra'}
   result[name+'_sizes'][n]=dict(total=sum(secs.values()),extra=secs.get('.live_extra',0))
assert result['base_abi']==result['candidate_abi'];result['abi_equal']=True;result['abi_fields']=fields
result['object_growth_total']=sum(x['total'] for x in result['candidate_sizes'].values())-sum(x['total'] for x in result['base_sizes'].values())
result['extra_growth']=sum(x['extra'] for x in result['candidate_sizes'].values())-sum(x['extra'] for x in result['base_sizes'].values())
result['primary_growth']=result['object_growth_total']-result['extra_growth']
(p/'abi-size.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
