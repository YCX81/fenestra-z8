#!/usr/bin/env python3
"""v7 linked AArch64 busy-loop tests. Native scheduling/time are explicit inputs.
Unicorn only, no QEMU or devices. Checkpoint ownership/encoding executes for real.
"""
import json,struct,subprocess,hashlib,io,sys
from pathlib import Path
support=Path(__file__).with_name('test_review9.py')
exec(compile(support.read_text().split('rows=[]')[0],str(support),'exec'))
from video_decoder import parse_bits
from test_video_decoder import bits_for
names7=['busy','release_req','preempts','eos_miss','batch_open']
p=link/'v7-layout.c';p.write_text('#include <stddef.h>\n#include "codec/live_w5enc.c"\nconst unsigned offsets7[]={'+','.join('offsetof(struct live_w5,'+n+')' for n in names7)+'};\n')
subprocess.run(['clang','--target=aarch64-none-elf','-ffreestanding','-mgeneral-regs-only','-I',str(build),'-I',str(build/'codec/freestanding'),'-I',str(build/'ui/core/include'),'-c',str(p),'-o',str(link/'v7-layout.o')],check=True)
e=ELFFile(io.BytesIO((link/'v7-layout.o').read_bytes()));sym=next(s for s in e.get_section_by_name('.symtab').iter_symbols() if s.name=='offsets7');sec=e.get_section(sym['st_shndx']);o7=dict(zip(names7,struct.unpack('<5I',sec.data()[sym['st_value']:sym['st_value']+20])))
class BusyMachine(Machine):
 def __init__(self,clear=None,duration=1000000,step=1000,alive=1,busy=1,clear_in_frame=None,stall=False):
  self.wait=0;self.clear=clear;self.step=step;self.alive=alive;self.clear_in_frame=clear_in_frame;self.stall=stall;self.stopped=False;self.events=[]
  super().__init__();self.duration=duration;self.w32(HW+o7['busy'],busy)
 def hook(self,u,a,n,_):
  if a==sy['alive']:
   self.ret(self.alive if not any(e['close_stage']==2 for e in self.events) else 0);return
  if a==0x40187b00:
   if self.stall:self.stopped=True;u.emu_stop();return
   self.time+=self.step;self.wait+=self.step
   if self.clear is not None and self.wait>=self.clear:self.w32(HW+o7['busy'],0)
   self.ret();return
  if a==sy['emit_frame']:
   value=u.reg_read(xr[1]);row=parse_bits(bits_for(4,value),short=True);assert row['accepted'],row
   row.update(wait_us=self.wait,time_us=self.time,failed=self.get(HW+off['close_failed']));self.events.append(row)
   if row['close_stage']==self.clear_in_frame:self.w32(HW+o7['busy'],0)
  super().hook(u,a,n,_)
 def preempt(self):self.run('live_encoder_preempt_lamp',[ENC,0,STATE,REGS])
rows=[]
for duration in (0,1000000,1250000,1370000,5000000):
 for clear in (None,500000,1000000,1001000,2999000,3000000,3999000,4000000,4001000):
  m=BusyMachine(clear,duration);m.preempt()
  timeout=clear is None or clear>4000000
  end=4000000 if timeout else clear
  assert m.wait==end,(duration,clear,m.wait)
  beats=[e for e in m.events if e['close_stage']==3]
  assert [e['wait_us'] for e in beats]==list(range(1000000,min(end,3999999)+1,1000000))
  for e in beats:
   assert e['waited_seconds']==e['wait_us']//1000000
   assert e['busy']==int(clear is None or e['wait_us']<clear)
  assert m.events[0]['close_stage']==1 and (m.events[0]['busy'],m.events[0]['alive'])==(1,1)
  assert m.events[-1]['close_stage']==(4 if timeout else 2)
  assert m.get(HW+off['close_failed'])==int(timeout)
  assert m.get(HW+off['fin_rc'])>>24==(10 if timeout else 0)
  assert m.get(HW+o7['release_req'])==0 and m.get(HW+o7['preempts'])==1
  assert m.time==m.wait+len(m.events)*duration
  assert all(e['failed']==0 for e in m.events)
  rows.append(dict(duration=duration,clear=clear,wait_us=m.wait,wall_us=m.time,stages=[e['close_stage'] for e in m.events]))
# Delayed scheduling skips missed beats (no catch-up burst), and still expires
# at the first executed old-style condition at/after four non-emission seconds.
for step in (1100000,2500000,4500000):
 m=BusyMachine(step=step);m.preempt();assert m.wait==((4000000+step-1)//step)*step
 assert [e['waited_seconds'] for e in m.events if e['close_stage']==3]==[t//1000000 for t in range(step,4000000,step)]
 rows.append(dict(step=step,events=m.events))
for phase in (1,3,4):
 m=BusyMachine(clear_in_frame=phase);m.preempt()
 assert [e['close_stage'] for e in m.events]==({1:[1,2],3:[1,3,2],4:[1,3,3,3,4]}[phase])
 assert m.get(HW+off['close_failed'])==int(phase==4) # timeout decision remains latched
 rows.append(dict(clear_during=phase,events=m.events))
for busy in (0,1):
 m=BusyMachine(alive=0,busy=busy);m.preempt()
 assert [e['close_stage'] for e in m.events]==[1,5]
 assert (m.events[0]['busy'],m.events[0]['alive'])==(busy,0)
 assert m.wait==0 and m.get(HW+o7['release_req'])==0 and m.get(HW+o7['preempts'])==0
 rows.append(dict(not_alive=True,busy=busy,events=m.events))
m=BusyMachine(busy=0);m.preempt();assert [e['close_stage'] for e in m.events]==[1,2] and m.wait==0
m=BusyMachine(stall=True)
try:m.preempt()
except AssertionError:assert m.stopped
else:raise AssertionError('blocked DELAY unexpectedly returned')
assert [e['close_stage'] for e in m.events]==[1]
# Every new checkpoint rejects foreign session/stack/LR and cancellation before
# lamp ownership, while valid calls encode real D bits and never touch slots.
from unicorn import UC_HOOK_MEM_WRITE
for stage,detail in [(1,v) for v in range(4)]+[(3,v) for v in range(8)]+[(4,1),(5,0)]:
 for fault in ('none','session','stack','lr','no_i','returned','done','unarmed'):
  m=Machine();writes=[]
  if fault=='lr':m.w64(REGS+240,0x40cf9858)
  if fault=='no_i':m.w32(STATE+52,0)
  if fault=='returned':m.w32(STATE+28,1<<29)
  if fault=='done':m.w32(STATE+24,0x80000000)
  if fault=='unarmed':m.w32(STATE,0)
  m.u.hook_add(UC_HOOK_MEM_WRITE,lambda u,ac,a,n,v,_:writes.append(a),begin=STATE+28,end=STATE+31)
  slots=bytes(m.u.mem_read(STATE+off['diag.close_owner'],104))
  m.run('fr_checkpoint_value',[STATE+16 if fault=='session' else STATE,REGS,stage,detail],sp=TOP-784-(0x10000 if fault=='stack' else 0))
  assert bool(m.emits)==(fault=='none'),(stage,detail,fault,m.emits)
  assert slots==bytes(m.u.mem_read(STATE+off['diag.close_owner'],104))
  if fault!='none':assert not writes
  else:
   value=m.emits[0][1];row=parse_bits(bits_for(4,value),short=True);assert row['accepted'] and row['close_stage']==stage
   if stage==3:assert (row['busy'],row['waited_seconds'])==(detail&1,detail>>1)
report=dict(main_sha256=hashlib.sha256((link/'live-fixed.emulation-only.main').read_bytes()).hexdigest(),wait_cases=rows,identity_cases=112,limits='Linked AArch64 instructions under Unicorn; native calls, clock and task resumption stubbed. No hardware or QEMU. Simulated non-returning DELAY demonstrates observation limit, not the real reboot cause.')
(link/'v7-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: 50 timed busy cases, 3 scheduling jumps, 3 in-frame clears, !alive/busy=0/blocked DELAY; 112 new-checkpoint identity/encoding cases')
