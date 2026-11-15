from pathlib import Path
import subprocess,struct,json
H=Path(__file__).resolve().parent;W=H/'work/src-fix37';O=H/'objects';O.mkdir(exist_ok=True)
flags=['--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-mgeneral-regs-only','-mstrict-align','-DLA_WIDTH=1024','-DLA_HEIGHT=680','-DLA_RING_CAPACITY=96','-DLA_SAMPLE_INTERVAL_US=25000','-DLIVE_BITRATE=8000000','-DLIVE_IDR_US=500000','-DLIVE_ENCODE_SPEED=9','-Wall','-Wextra','-Werror','-I',str(W),'-I',str(W/'ui/core/include'),'-I',str(W/'codec/freestanding')]
report={}
for name,old,new in [('live_stream',H/'live_stream.before.c',W/'capture/live_stream.c'),('live_mov',H/'live_mov.before.c',W/'codec/live_mov.c')]:
 nums=[]
 for lab,src in [('old',old),('new',new)]:
  obj=O/f'{name}-{lab}.o';subprocess.run(['clang',*flags,'-I',str(new.parent),'-c',str(src),'-o',str(obj)],check=True)
  b=obj.read_bytes();off=struct.unpack_from('<Q',b,40)[0];sz,n=struct.unpack_from('<HH',b,58);total=0
  for i in range(n):
   s=struct.unpack_from('<IIQQQQIIQQ',b,off+sz*i)
   if s[2]&2:total+=s[5]
  nums.append(total)
 report[name]={'old_allocated_sections':nums[0],'new_allocated_sections':nums[1],'delta':nums[1]-nums[0]}
report['total_delta']=sum(x['delta'] for x in report.values());(H/'object-size.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
