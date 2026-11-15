#!/usr/bin/env python3
"""Actual delivered-G SFX queue rejection/cancel ownership and completion.

Original constructor/dispatcher/SoundTask callback all execute. OS queue and
heap are instrumented, sound/DAC start is not emulated. No firmware is patched.
"""
from pathlib import Path
import hashlib, json, struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
P = Path(__file__).resolve().parent
D = (P.parent/'v52g.main').read_bytes(); B = 0x40160000
assert hashlib.sha256(D).hexdigest() == 'ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'
u = Uc(UC_ARCH_ARM64, UC_MODE_ARM); u.mem_map(B, (len(D)+4095)&~4095); u.mem_write(B,D)
for a,n in [(0x4e000000,0x200000),(0xa038fe000,0x2000),(0xa02d4c000,0x1000),(0xa03bc6000,0x2000)]:u.mem_map(a,n)
END, STACK, HEAP = 0x4e001000, 0x4e03f000, 0x4e100000
r32=lambda a:struct.unpack('<I',u.mem_read(a,4))[0]
r64=lambda a:struct.unpack('<Q',u.mem_read(a,8))[0]
w32=lambda a,v:u.mem_write(a,struct.pack('<I',v&0xffffffff))
heap=HEAP; queued=[]; allocated={}; freed=[]; events=[]; trace=[]
def ret(v=None):
 if v is not None:u.reg_write(UC_ARM64_REG_X0,v)
 u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(uc,pc,size,data):
 global heap
 if pc==0x40d2c408:
  n=u.reg_read(UC_ARM64_REG_W0);a=heap;heap=(heap+n+15)&~15;u.mem_write(a,bytes(n));allocated[a]=n;ret(a)
 elif pc==0x40d2c448:
  a=u.reg_read(UC_ARM64_REG_X0);assert a in allocated and a not in freed,hex(a);freed.append(a);ret()
 elif pc==0x40c00bac:queued.append(u.reg_read(UC_ARM64_REG_X1));ret(1)
 elif pc==0x40c7c778:u.mem_write(u.reg_read(UC_ARM64_REG_X0),struct.pack('<Q',1000000));ret(0)
 elif pc==0x40e31718:ret(100) # tone duration lookup only; no ownership effect
 elif pc in (0x40f9ddd0,0x40f9de70,0x40f9de20):ret(0) # passive logging
 elif pc==0x40d2bc8c:events.append([hex(u.reg_read(UC_ARM64_REG_X0)),u.reg_read(UC_ARM64_REG_W1)]);ret(0)
 elif pc in (0x40e3159c,0x40e32ed8,0x40f9d194):
  p=u.reg_read(UC_ARM64_REG_X1);trace.append(dict(pc=hex(pc),event=hex(u.reg_read(UC_ARM64_REG_W0)),arg=hex(p),words=[r32(p),r32(p+4)]))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc,*args):
 u.reg_write(UC_ARM64_REG_SP,STACK);u.reg_write(UC_ARM64_REG_LR,END)
 for i,v in enumerate(args):u.reg_write(UC_ARM64_REG_X0+i,v)
 u.emu_start(pc,END,count=100000)
 assert u.reg_read(UC_ARM64_REG_PC)==END,hex(u.reg_read(UC_ARM64_REG_PC))
 return u.reg_read(UC_ARM64_REG_W0)
def reset():
 global heap
 for a,n in [(0xa038fe000,0x2000),(0xa02d4c000,0x1000),(0xa03bc6000,0x2000)]:u.mem_write(a,bytes(n))
 w32(0xa038fea44,3);w32(0xa02d4cb48,5)
 heap=HEAP;queued.clear();allocated.clear();freed.clear();events.clear();trace.clear()
def state():return dict(pending=u.mem_read(0xa038fe9f1,1)[0],soundtask=r32(0xa03bc7010),tone=r32(0xa03bc700c),sfx=r32(0xa038feb74),deferred_cb=hex(r64(0xa038febb8)),allocated=len(allocated),freed=len(freed))
rows=[]
for tone in (1,2):
 reset();call(0x40f9ce60,tone,0);before=state();assert len(queued)==1 and before['pending']==1 and before['soundtask']==1
 msg=queued.pop(0);assert r32(msg+32)==0x04100030
 call(0x40e34b2c,msg);after=state()
 assert after['pending']==after['soundtask']==after['sfx']==0 and after['allocated']==after['freed']==3
 assert [r['event'] for r in trace if r['pc']=='0x40f9d194']==['0x4102004','0x4102005']
 rows.append(dict(case='actual driver5 rejection',tone=tone,before=before,after=after,trace=trace[:],events=events[:]))

# Retaining a start envelope outside the native queue changes cancel ordering.
# Actual native 031 by itself cannot withdraw that unsubmitted envelope.
reset();call(0x40f9ce60,1,0);held=queued.pop(0);assert call(0x40e31540)==1
stop=queued.pop(0);assert r32(stop+32)==0x04100031
call(0x40e34b2c,stop);after_stop=state();assert after_stop['sfx']==2 and after_stop['pending']==1 and after_stop['soundtask']==1
call(0x40e34b2c,held);after_held=state();assert after_held['soundtask']==0 and after_held['pending']==1 and after_held['sfx']==2
rows.append(dict(case='counterexample: native031 before privately held030',after_stop=after_stop,after_rejected_held=after_held,trace=trace[:],events=events[:]))

# A complete native rejection while driver5 is still the owned capture owner
# does unwind the envelope without changing native sound/driver globals.
# It is valid only before a native031/other SFX state transition, not arbitrary
# cancellation after queue transfer or after another sound owner starts.
reset();call(0x40f9ce60,2,0);held=queued.pop(0)
call(0x40e34b2c,held);s=state();assert s['pending']==s['soundtask']==s['sfx']==0 and s['allocated']==s['freed']==3
rows.append(dict(case='withheld envelope cancellation via actual still-capture rejection',after=s,driver=r32(0xa02d4cb48),trace=trace[:]))

report=dict(passed=True,firmware_sha256=hashlib.sha256(D).hexdigest(),cases=rows,
 boundary='Heap and OS queue are shims; native allocation wrappers, SFX dispatch, outer completion and SoundTask callback execute. Tone duration and passive logs are stubs. No admitted DAC playback or physical cancellation timing is asserted.',
 conclusions=['Normal tone1/2 driver5 rejection invokes inner2004/2005 and outer2000; clears pending/SoundTask and frees all3 heap objects.',
 'Native031 does not identify or remove a privately held030; dispatching031 first sets SFXstate2 and leaves pending1 after late030 rejection. It is not a safe cancellation adapter.',
 'A held030 may be rejected through the original complete path while driver5/SFXidle still apply; this does not generalize to a transferred native queue or stopped microphone.'])
(P/'sound-cancel.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS actual normal-pair rejection full callbacks/frees; native031-before-held030 counterexample retained')
