#!/usr/bin/env python3
"""Original SFX accepted/STOP state protocol with explicit lower-service shims.
Does not claim actual DMA/audio or timing; compares callback/ownership grammar.
"""
from pathlib import Path
import json
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;ns={'__file__':str(P/'test_sound_cancel.py')}
exec(compile((P/'test_sound_cancel.py').read_text().split('rows=[]')[0],str(P/'test_sound_cancel.py'),'exec'),ns)
u=ns['u'];call=ns['call'];reset=ns['reset'];state=ns['state'];w32=ns['w32'];r32=ns['r32'];r64=ns['r64']
low=[];outer=[]
def extra(uc,pc,size,data):
 if pc in (0x40e333a8,0x40e33e04,0x40e33838,0x40e33440,0x40e334d4):
  low.append(hex(pc))
  if pc==0x40e33440:w32(0xa02d4cb48,3)
  if pc==0x40e334d4:w32(0xa02d4cb48,1)
  ns['ret'](1)
 elif pc==0x40e340fc:ns['ret'](0)
 elif pc==0x40e34114:ns['ret'](39)
 elif pc==0x40e348f4:ns['ret'](9600)
 elif pc==0x40e3469c:ns['ret'](0x4e050000)
 elif pc==0x40e3159c:outer.append(dict(event=hex(uc.reg_read(UC_ARM64_REG_W0)),status=r32(uc.reg_read(UC_ARM64_REG_X1)+16)))
u.hook_add(UC_HOOK_CODE,extra)
rows=[]
for tone in (1,2):
 reset();low.clear();outer.clear();w32(0xa02d4cb48,1)
 call(0x40f9ce60,tone,0);call(0x40e30d18,ns['queued'].pop(0))
 started=state();assert started['sfx']==started['pending']==started['soundtask']==1 and started['freed']==3
 inner=[x for x in ns['trace']if x['pc']=='0x40f9d194'];assert [x['event']for x in inner]==['0x4102004'] and inner[0]['words']==[tone,0]
 assert outer==[dict(event='0x4102000',status=0)]
 assert call(0x40e316bc)==1;call(0x40e30d18,ns['queued'].pop(0));stopped=state()
 assert stopped['sfx']==stopped['pending']==stopped['soundtask']==0 and stopped['allocated']==stopped['freed']==5
 assert outer==[dict(event='0x4102000',status=0),dict(event='0x4102002',status=0)]
 assert [x['event']for x in ns['trace']if x['pc']=='0x40f9d194']==['0x4102004','0x4102005']
 rows.append(dict(tone=tone,started=started,stopped=stopped,lower_shims=low[:],outer=outer[:],trace=ns['trace'][:]))
(P/'sound-accepted.json').write_text(json.dumps(dict(passed=True,cases=rows,boundary='Original SFX manager30d18/dispatcher/accepted handler/STOP handler/state/callback cleanup execute. Lower OPEN/output/buffer/START/STOP and sample resource getters are success shims; no actual service/DMA sound/timing is claimed.',conclusion='inner2004 payload second word0 occurs for accepted start too; outer status0 + SFXstate1 distinguish it. Hard STOP032 after actual admission calls lower STOP, clearsSFXstate, emits2005 and completesouter2002; unlike privately held start, this cancel must remain native.'),indent=2)+'\n')
print('PASS actual SFX accepted then native032 protocol; lower hardware service is explicitly shimmed')
