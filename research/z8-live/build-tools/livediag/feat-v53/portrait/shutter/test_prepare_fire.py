#!/usr/bin/env python3
"""Run normal one-shot prepare/fire producers and task through full rejection.
The delivered image is read-only. Camera settings/exposure time/OS facilities
are fixture inputs; native producer/task/SFX/callback code itself executes.
"""
from pathlib import Path
import json,struct
P=Path(__file__).resolve().parent
source=(P/'test_sound_cancel.py').read_text().split('rows=[]')[0]
ns={'__file__':str(P/'test_sound_cancel.py')};exec(compile(source,str(P/'test_sound_cancel.py'),'exec'),ns)
u=ns['u'];D=ns['D'];B=ns['B'];call=ns['call'];r32=ns['r32'];w32=ns['w32'];reset=ns['reset'];state=ns['state']
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import *
for a in (0xa0292b000,0xa029ca000):
 u.mem_map(a,4096);off=a-0xa02808000+0x4b13000;u.mem_write(a,D[off:off+4096])
flags=[];hits=[]
def extra(uc,pc,size,data):
 if pc in (0x41012c84,0x40c784dc,0x40c784f4,0x40c78570,0x40d2bcc8,0x40d2b88c):ns['ret'](0)
 elif pc==0x40f94f5c:ns['ret'](2) # no prepare timer delay for this fixture
 elif pc==0x401e599c:
  # Existing Live mute hook is deliberately an input here: stock/effective mute0.
  uc.reg_write(UC_ARM64_REG_W8,uc.mem_read(0xa03bc7008,1)[0]);ns['ret']()
 elif pc==0x40c002a0:flags.append(uc.reg_read(UC_ARM64_REG_W1));ns['ret'](0)
 elif pc in (0x40f9caf4,0x40f9cc04):hits.append(dict(pc=hex(pc),tone=uc.reg_read(UC_ARM64_REG_W0),duration=uc.reg_read(UC_ARM64_REG_W1)))
 elif pc in (0x40d2bd04,0x40d2bd64):raise AssertionError('Unexpected blocking wait in immediate normal-shot fixture')
u.hook_add(UC_HOOK_CODE,extra)
reset();w32(0xa03bc700c,60)
arg=0x4e005000;u.mem_write(arg,struct.pack('<QQQ',0,1,1000000))
call(0x40f9cccc,arg);assert flags==[2]
call(0x40f9c5cc,2);a=state();assert [r32(0xa0292be18),r32(0xa0292be1c)]==[1,2]
assert len(ns['queued'])==1;call(0x40e34b2c,ns['queued'].pop(0));b=state()
call(0x40f9cd54,0);assert flags==[2,4]
call(0x40f9c5cc,4);c=state();assert len(ns['queued'])==1;call(0x40e34b2c,ns['queued'].pop(0));d=state()
assert hits==[dict(pc='0x40f9caf4',tone=1,duration=0),dict(pc='0x40f9cc04',tone=2,duration=0)]
assert a['pending']==a['soundtask']==c['pending']==c['soundtask']==1
assert b['pending']==b['soundtask']==d['pending']==d['soundtask']==0
assert d['allocated']==d['freed']==6
report=dict(passed=True,producer_event_flags=flags,call_sites=hits,prepare_queued=a,prepare_complete=b,fire_queued=c,fire_complete=d,callback_trace=ns['trace'],
 boundary='Normal tone-set0, immediate timing and effective mute0 are fixtures. Setting/exposure/OS interfaces and duration/logging are shims; producer writes, normal task selection, complete constructor/dispatcher/real SoundTask callbacks execute. Native accepted sound/DAC and bulb/loop paths are not claimed.',
 callgraph='40f9cccc stores prepare data + event2; 40f9c5cc selects1/2 ->caf4 ->ce60 ->312d8 ->3133c ->32e6c; mic5 rejection ->inner2004/2005 ->outer2000 cleanup. 40f9cd54 stores mute + event4; 40f9c5cc ->cb64 gate ->cc04 ->same chain for tone2.',
 return_semantics='40e3133c and 40e312d8 return queue-admission bool. 40f9ce60 ignores it, updates deadline/tone and returns logger-derived incidental w0; prepare/fire callers also ignore it. A hook cannot use ce60 return as a successful sound completion signal.')
(P/'prepare-fire.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS actual normal prepare/fire pair, complete mic5 rejection/callback cleanup; no event wait in fixture')
