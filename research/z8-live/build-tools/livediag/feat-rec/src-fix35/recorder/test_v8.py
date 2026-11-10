#!/usr/bin/env python3
"""v8 linked AArch64 busy-loop tests. Native scheduling/time are explicit inputs.
Unicorn only, no QEMU or devices. Checkpoint ownership/encoding executes for real.
"""
import json,struct,subprocess,hashlib,io,sys
from pathlib import Path
support=Path(__file__).with_name('test_review9.py')
exec(compile(support.read_text().split('rows=[]')[0],str(support),'exec'))
from video_decoder import parse_bits
from test_video_decoder import bits_for
BaseMachine=Machine
MODE=0xa03bd062c
class Machine(BaseMachine):
 def __init__(self):
  super().__init__();self.u.mem_map(MODE&~4095,4096);self.w32(MODE,1)
names7=['busy','release_req','preempts','eos_miss','batch_open']
p=link/'v7-layout.c';p.write_text('#include <stddef.h>\n#include "codec/live_w5enc.c"\nconst unsigned offsets7[]={'+','.join('offsetof(struct live_w5,'+n+')' for n in names7)+'};\n')
subprocess.run(['clang','--target=aarch64-none-elf','-ffreestanding','-mgeneral-regs-only','-I',str(build),'-I',str(build/'codec/freestanding'),'-I',str(build/'ui/core/include'),'-c',str(p),'-o',str(link/'v7-layout.o')],check=True)
e=ELFFile(io.BytesIO((link/'v7-layout.o').read_bytes()));sym=next(s for s in e.get_section_by_name('.symtab').iter_symbols() if s.name=='offsets7');sec=e.get_section(sym['st_shndx']);o7=dict(zip(names7,struct.unpack('<5I',sec.data()[sym['st_value']:sym['st_value']+20])))
class ProgressMachine(Machine):
 def __init__(self,busy=0,active=1,clear=None,stall=False):
  self.active=active;self.wait=0;self.clear=clear;self.stall=stall;self.blocked=False
  super().__init__();self.w32(HW+o7['busy'],busy)
 def hook(self,u,a,n,_):
  if a==sy['alive']:self.ret(self.active);return
  if a==0x40187b00:
   if self.stall:self.blocked=True;u.emu_stop();return
   self.wait+=1000
   if self.clear is not None and self.wait>=self.clear:self.w32(HW+o7['busy'],0)
  super().hook(u,a,n,_)
rows=[]
for clear in (0,1000,500000,1000000,3999000,4000000,None):
 m=ProgressMachine(busy=int(clear!=0),clear=clear);m.clear_after=0
 m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
 assert not m.emits
 assert m.wait==(4000000 if clear is None else clear),(clear,m.wait)
 stages=[p[0] for p in m.points]
 assert stages==sorted(stages),stages
 assert (11 in stages)==(clear is None)
 assert (12 in stages)==(clear is not None)
 assert m.get(HW+o7['release_req'])==0 and m.get(HW+o7['preempts'])==1
 assert m.get(HW+off['close_failed'])==int(clear is None)
 rows.append(dict(clear=clear,wait=m.wait,points=len(m.points),last=stages[-1]))
m=ProgressMachine(active=0);m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS]);assert m.points[-1][0]==58 and not m.wait
# Freeze MOVRS at native DELAY, then independently run production worker tick.
m=ProgressMachine(busy=1,stall=True)
try:m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
except AssertionError:assert m.blocked
else:raise AssertionError('DELAY unexpectedly returned')
assert m.get(STATE+off['diag.packed'])==9<<3|1
for i in range(10):
 m.time=(i+1)*2500000;m.run('fr_worker_tick',[STATE],sp=TOP+0x8000)
 assert m.emits[-1][0]==5 and m.emits[-1][1]==(9<<6|1<<3|(i&7))
assert len(m.emits)==10
for fault in ('session','stack','lr','no_i','returned','done','unarmed'):
 m=Machine()
 if fault=='lr':m.w64(REGS+240,0x140d07ba8)
 if fault=='no_i':m.w32(STATE+off['diag.i_done'],0)
 if fault=='returned':m.w32(STATE+28,1<<29)
 if fault=='done':m.w32(STATE+24,0x80000000)
 if fault=='unarmed':m.w32(STATE,0)
 m.run('fr_progress',[STATE+16 if fault=='session' else STATE,REGS,9,1,1,1,0],sp=TOP-784-(0x10000 if fault=='stack' else 0))
 assert not m.points and not m.emits
# Worker never races I; reporter continues after R/M; exact session mandatory.
for fault in ('no_i','unclaimed','unarmed','foreign','returned','done'):
 m=Machine();m.time=3000000
 if fault=='no_i':m.w32(STATE+off['diag.i_done'],0)
 if fault=='unclaimed':m.w32(STATE+12,0)
 if fault=='unarmed':m.w32(STATE,0)
 if fault=='returned':m.w32(STATE+28,1<<29)
 if fault=='done':m.w32(STATE+24,0x80000000)
 m.run('fr_worker_tick',[STATE+16 if fault=='foreign' else STATE],sp=TOP+0x8000)
 assert bool(m.emits)==(fault in ('returned','done'))
# Execute both native output/drain timeout branches: explicit RAM timeout,
# original error code and no diagnostic DELAYs beyond the native wait loop.
class TimeoutMachine(ProgressMachine):
 def __init__(self,mode):self.mode=mode;super().__init__()
 def hook(self,u,a,n,_):
  if a==0x40c33fb0 and (self.mode=='eos' or self.triggers==1):
   self.triggers+=1;self.ret();return
  super().hook(u,a,n,_)
for mode,stage,code,wait in [('eos',26,5,501000),('drain',32,7,1001000)]:
 m=TimeoutMachine(mode);m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
 assert (stage,4,wait) in m.points,(mode,m.points[-12:])
 assert m.wait==wait and not m.emits and m.get(HW+off['fin_rc'])>>24==code
 rows.append(dict(timeout=mode,wait_us=wait,stage=stage,error=code))
# REVIEW14 P1: nonzero modes (not just 1), claim/I conjunction and terminal flags.
for mode in (0,1,2,0xffffffff):
 for claimed in (0,1):
  for i_done in (0,1):
   for ended in ('none','returned','done'):
    m=Machine();m.time=3000000;m.w32(MODE,mode)
    m.w32(STATE+12,claimed);m.w32(STATE+off['diag.i_done'],i_done)
    if ended=='returned':m.w32(STATE+28,1<<29)
    if ended=='done':m.w32(STATE+24,0x80000000)
    before=bytes(m.u.mem_read(STATE,3072));t=m.time
    m.run('fr_worker_tick',[STATE],sp=TOP+0x8000)
    allowed=bool(mode and claimed and i_done)
    assert bool(m.emits)==allowed,(mode,claimed,i_done,ended)
    if not allowed:assert m.time==t and bytes(m.u.mem_read(STATE,3072))==before
# Stop at each real blocking native call. Compare actual RAM and P payloads.
class BlockMachine(ProgressMachine):
 def __init__(self,target):
  self.target=target;self.blocked=False
  super().__init__();self.clear_after=0
  if target=='submit':self.w32(HW+off['frames'],0)
  if target=='lock_delay':self.w32(LOCK,1)
  if target=='busy_delay':self.w32(HW+o7['busy'],1)
 def hook(self,u,a,n,_):
  if a==sy['memcpy']:
   dst,src,count=(u.reg_read(xr[i]) for i in range(3));u.mem_write(dst,bytes(u.mem_read(src,count)));self.ret(dst);return
  stage=self.get(STATE+off['diag.packed'])>>3
  calls={'stop':0x40c34078,'trigger':0x40c33fb0,'drain_trigger':0x40c33fb0,
         'finish':0x40c340e8,'exit':sy['fr_live_exit']}
  # SUBMIT address is read from the generated codec's macro below.
  calls['submit']=submit_address
  delays={'busy_delay':9,'lock_delay':16,'output_delay':26,'drain_delay':32,'vdi_delay':44}
  hit=(a==calls.get(self.target) and (self.target!='drain_trigger' or self.triggers==1))
  hit|=a==0x40187b00 and stage==delays.get(self.target)
  if hit:self.blocked=True;u.emu_stop();return
  if a==0x40c33fb0 and ((self.target=='output_delay' and self.triggers==0) or (self.target=='drain_delay' and self.triggers==1)):
   self.triggers+=1;self.ret();return
  if a==sy['fr_live_exit'] and self.target=='vdi_delay':self.ret();return
  super().hook(u,a,n,_)
import re
submit_address=int(re.search(r'#define SUBMIT FN\((0x[0-9a-f]+)',(build/'codec/live_w5enc.c').read_text())[1],16)
blocked=[]
for target,stage in [('busy_delay',9),('lock_delay',16),('submit',20),('stop',22),('trigger',24),('output_delay',26),('drain_trigger',30),('drain_delay',32),('finish',36),('exit',40),('vdi_delay',44)]:
 m=BlockMachine(target)
 try:m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
 except AssertionError:assert m.blocked,target
 except Exception as e:raise RuntimeError((target,hex(m.u.reg_read(r.UC_ARM64_REG_PC)),m.points[-5:])) from e
 else:raise AssertionError(('native call did not block',target))
 assert m.get(STATE+off['diag.packed'])>>3==stage,(target,m.points[-8:])
 m.time=3000000;m.run('fr_worker_tick',[STATE],sp=TOP+0x8000)
 decoded=parse_bits(bits_for(5,m.emits[-1][1]),True,'v8')
 assert decoded['accepted'] and decoded['progress_stage']==stage and decoded['progress_label']!='未分配阶段'
 blocked.append(dict(block=target,stage=stage,label=decoded['progress_label']))
assert len({r['stage'] for r in blocked})==len(blocked)
# Both successful TRIGGER returns and EXIT return have their own monotonic points.
m=ProgressMachine();m.clear_after=0;m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
stages=[p[0] for p in m.points]
assert stages==sorted(stages) and all(v in stages for v in (24,25,30,31,40,41)),stages
# Native TRIGGER error returns still publish their return point before cleanup.
class TriggerErrorMachine(ProgressMachine):
 def __init__(self,nth):self.nth=nth;super().__init__()
 def hook(self,u,a,n,_):
  if a==0x40c33fb0 and self.triggers+1==self.nth:
   self.triggers+=1;self.ret(1);return
  super().hook(u,a,n,_)
for nth,stage,error in ((1,25,4),(2,31,6)):
 m=TriggerErrorMachine(nth);m.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
 assert stage in [p[0] for p in m.points] and m.get(HW+off['fin_rc'])>>24==error
rows.extend(blocked)
report=dict(cases=rows,main_sha256=hashlib.sha256((link/'live-fixed.emulation-only.main').read_bytes()).hexdigest(),limits='Unicorn only. Scheduler/native calls stubbed. Independent invocation proves software separation, NOT that stock REC leaves worker runnable.')
(link/'v8-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: RAM-only busy=0/timeout/clear/!alive, monotonic stages, stalled MOVRS with 10 worker reports, identity and I ownership')
