"""Compile actual private adapter; run it with delivered G native SFX/callbacks.
Only external Live registry/mic backend and lower DAC service are shims.
"""
from pathlib import Path
import hashlib,io,json,struct,subprocess
from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent;S=P;W=P/'work'
ns={'__file__':str(S/'test_sound_cancel.py')}
exec(compile((S/'test_sound_cancel.py').read_text().split('rows=[]')[0],str(S/'test_sound_cancel.py'),'exec'),ns)
u=ns['u'];call=ns['call'];r32=ns['r32'];r64=ns['r64'];w32=ns['w32'];native_reset=ns['reset'];state=ns['state'];ret=ns['ret']
ns['queued'].clear()
flags=['clang-21','--target=aarch64-none-elf','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-mgeneral-regs-only','-mstrict-align','-Wall','-Wextra','-Werror','-I'+str(W),'-I'+str(W/'ui/core/include'),'-I'+str(W/'codec/freestanding')]
subprocess.run(flags+['-c',str(W/'audio/live_shutter_z8.c'),'-o',str(P/'shutter.o')],check=True)
expr={'size':'sizeof(struct live_runtime)','magic':'offsetof(struct live_runtime,magic)','requested':'offsetof(struct live_runtime,requested)','shutdown':'offsetof(struct live_runtime,shutdown)','hold':'offsetof(struct live_runtime,crumb_pad)','active':'offsetof(struct live_runtime,audio.active)','cancelled':'offsetof(struct live_runtime,audio.cancelled)','frozen':'offsetof(struct live_runtime,audio.frozen)','warm':'offsetof(struct live_runtime,audio_warm)','error':'offsetof(struct live_runtime,audio_error)','backend':'offsetof(struct live_runtime,audio_z8.phase)','ctx':'offsetof(struct live_runtime,shutter)'}
for x in ['held','began','phase','generation','tone','wake','wake_busy','wake_generation','wake_cancel','cancel_posted','captured','played','completed','cancelled','errors']:
 expr['s_'+x]='offsetof(struct live_runtime,shutter.'+x+')'
(P/'offsets.c').write_text('#include "runtime.h"\nconst unsigned long offsets[]={'+','.join(expr.values())+'};\n')
subprocess.run(flags+['-c',str(P/'offsets.c'),'-o',str(P/'offsets.o')],check=True)
e=ELFFile(io.BytesIO((P/'offsets.o').read_bytes()));sy=e.get_section_by_name('.symtab');v=next(s for s in sy.iter_symbols()if s.name=='offsets');b=e.get_section(v['st_shndx']).data()[v['st_value']:][:8*len(expr)];O=dict(zip(expr,struct.unpack('<'+'Q'*len(expr),b)))
R=0x4e060000;BASE=0x44080000;STUB=0x44090000;u.mem_map(0xa03bd0000,0x2000)
e=ELFFile(io.BytesIO((P/'shutter.o').read_bytes()));sy=e.get_section_by_name('.symtab');sec=e.get_section_by_name('.live_shutter');code=bytearray(sec.data());stubnames=['live_ui_engine_get','live_shutter_mute_get','live_audio_clock_us','live_audio_z8_pause','live_audio_z8_resume','live_audio_cancel'];stubs={n:STUB+i*16 for i,n in enumerate(stubnames)}
for rel in e.get_section_by_name('.rela.live_shutter').iter_relocations():
 o=rel['r_offset'];t=rel['r_info_type'];sym=sy.get_symbol(rel['r_info_sym']);target=(stubs[sym.name] if sym['st_shndx']=='SHN_UNDEF' else BASE+sym['st_value'])+rel['r_addend'];ins=struct.unpack_from('<I',code,o)[0]
 if t in (282,283):ins=(ins&0xfc000000)|(((target-(BASE+o))>>2)&0x3ffffff)
 elif t==275:
  imm=((target&~4095)-((BASE+o)&~4095))>>12;ins=(ins&~0x60ffffe0)|((imm&3)<<29)|(((imm>>2)&0x7ffff)<<5)
 elif t==277:ins=(ins&~0x003ffc00)|((target&4095)<<10)
 else:raise AssertionError(t)
 struct.pack_into('<I',code,o,ins)
u.mem_write(BASE,bytes(code));symbols={s.name:BASE+s['st_value']for s in sy.iter_symbols()if isinstance(s['st_shndx'],int) and s['st_info']['type']=='STT_FUNC'}
ENTRY=symbols['live_shutter_dispatch'];STEP=symbols['live_shutter_step'];DONE=symbols['shutter_done']
# Exercise the exact single intended manager tail branch, leave original dispatch callable.
site=0x40e30e3c;old=struct.unpack('<I',u.mem_read(site,4))[0];assert old==0x14000f3c,hex(old)
u.mem_write(site,struct.pack('<I',0x14000000|(((ENTRY-site)//4)&0x3ffffff)))
now=1000000;mute=0;pauses=[];resumes=[];low=[];outer=[];post_fail=False;resume_fail=False;registry=True;resume_race=False;resume_partial=False;race_message=0;pause_error=0
# G constructor wraps queue0 as successful bool1; override old fixture's queue shim.
def extra(uc,pc,size,data):
 global post_fail
 if pc==0x40c00bac:
  if post_fail:ns['queued'].pop();post_fail=False;ret(1)
  else:ret(0)
 elif pc==stubs['live_ui_engine_get']:ret(R if registry else 0)
 elif pc==stubs['live_shutter_mute_get']:ret(mute)
 elif pc==stubs['live_audio_clock_us']:ret(now)
 elif pc==stubs['live_audio_z8_pause']:
  pauses.append(r32(R+O['backend']));b=r32(R+O['backend'])
  if pause_error:ret(pause_error&0xffffffff)
  elif b==0:ret(0)
  else:w32(R+O['backend'],3);ret(0xfffffff9)
 elif pc==stubs['live_audio_z8_resume']:
  resumes.append(now)
  if resume_fail:
   if resume_partial:w32(R+O['backend'],1);w32(0xa02d4cb48,4)
   ret(0xffffffff)
  else:w32(R+O['backend'],2);w32(0xa02d4cb48,5);ret(0)
  # Simulated native task publication during worker resume: real new message
  # fixture is captured separately below; CAS must not overwrite its phase.
  if resume_race:
   if race_message:
    u.mem_write(R+O['s_held'],struct.pack('<Q',race_message));w32(R+O['s_tone'],2);w32(R+O['s_generation'],r32(R+O['s_generation'])+1);u.mem_write(R+O['s_began'],struct.pack('<Q',now));w32(0xa03bc7010,1);u.mem_write(0xa038fe9f1,b'\1')
   w32(R+O['s_phase'],1)
 elif pc==stubs['live_audio_cancel']:w32(R+O['cancelled'],1);ret(0)
 elif pc in (0x40e333a8,0x40e33e04,0x40e33838,0x40e33440,0x40e334d4):
  low.append(hex(pc))
  if pc==0x40e33440:w32(0xa02d4cb48,3)
  if pc==0x40e334d4:w32(0xa02d4cb48,1)
  ret(1)
 elif pc==0x40e340fc:ret(0)
 elif pc==0x40e34114:ret(39)
 elif pc==0x40e348f4:ret(9600)
 elif pc==0x40e3469c:ret(0x4e050000)
 elif pc==0x40e3159c:outer.append(dict(event=hex(uc.reg_read(UC_ARM64_REG_W0)),status=r32(uc.reg_read(UC_ARM64_REG_X1)+16)))
u.hook_add(UC_HOOK_CODE,extra)
def get(name):return r32(R+O[name])
def set_(name,v):w32(R+O[name],v)
def reset():
 global now,mute,post_fail,resume_fail,registry,resume_race,resume_partial,race_message,pause_error
 native_reset();u.mem_write(R,bytes(O['size']));u.mem_write(0xa03bd0000,bytes(0x2000));w32(R,0x4c415231);w32(R+4,O['size']);set_('requested',1);set_('active',1);set_('backend',2)
 now+=1000000;mute=0;post_fail=False;resume_fail=False;registry=True;resume_race=False;resume_partial=False;race_message=0;pause_error=0;pauses.clear();resumes.clear();low.clear();outer.clear()
def create(tone=1):
 call(0x40f9ce60,tone,0);assert len(ns['queued'])==1;return ns['queued'].pop(0)
def deliver(m):return call(0x40e30d18,m)
def start(tone=1):
 m=create(tone);deliver(m);assert get('s_phase')==1 and r64(R+O['s_held'])==m;return m
def step(want=1,advance=0):
 global now
 now+=advance;return call(STEP,R,want,now)
def ack():set_('backend',0);w32(0xa02d4cb48,1)
def wake():assert len(ns['queued'])==1;deliver(ns['queued'].pop(0))
def stopped():assert call(0x40e316bc)==1;wake()
def admit(tone=1):
 start(tone);assert step()==1 and get('backend')==3 and not ns['queued'];ack();assert step()==1;wake();assert get('s_phase')==2 and state()['sfx']==1 and not resumes
rows=[]
# Actual adapter on the reachable post-rejection partial-start state.
reset();set_('s_phase',3);set_('backend',1);set_('cancelled',1);w32(0xa02d4cb48,4)
result=step();before=dict(return_value=result,phase=get('s_phase'),backend=get('backend'),driver=r32(0xa02d4cb48),pause_calls=pauses[:])
if 'LAZ8_STARTING||' in (W/'audio/live_shutter_z8.c').read_text().replace(' ',''):
 assert result==1 and get('backend')==3 and pauses==[1],before
 ack();assert step()==0 and get('s_phase')==0 and not resumes and get('cancelled')==1
 fixed=True
else:
 assert result==1 and get('backend')==1 and pauses==[],before
 fixed=False
rows.append(dict(case='partial-start remains after failed-resume/newholder/reject',fixed=fixed,observed=before))
# A fresh START may publish after worker took its now argument.
reset();start();u.mem_write(R+O['ctx']+8,struct.pack('<Q',now+1));step()
assert pauses and not ns['queued'] and get('s_phase')==1
rows.append(dict(case='began one tick later than worker now does not expire',passed=True))
report=dict(source_sha256=hashlib.sha256((W/'audio/live_shutter_z8.c').read_bytes()).hexdigest(),cases=rows,boundary='Actual adapter/native lookup and state branches; pause/stop ACK is the same explicit shim as upstream adapter test. Reachability is separately established by actual partial backend failure and adapter new-holder publication tests; no full scheduler simulation.')
(P/('review-fixed.json' if fixed else 'review-before.json')).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
