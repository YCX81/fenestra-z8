#!/usr/bin/env python3
"""ARM queue-order protocol against original native failure callbacks.
Only ownership/ready/cancel ordering is covered; no installed hook/DAC.
"""
from pathlib import Path
import io,json,struct,subprocess,hashlib
from elftools.elf.elffile import ELFFile
P=Path(__file__).resolve().parent
ns={'__file__':str(P/'test_sound_cancel.py')}
exec(compile((P/'test_sound_cancel.py').read_text().split('rows=[]')[0],str(P/'test_sound_cancel.py'),'exec'),ns)
u=ns['u'];call=ns['call'];reset=ns['reset'];state=ns['state'];r64=ns['r64'];w32=ns['w32']
def load(name,base):
 obj=P/(name+'.o');subprocess.run(['clang','--target=aarch64-none-elf','-std=c11','-O2','-fno-stack-protector','-fno-builtin','-c',str(P/(name+'.c')),'-o',str(obj)],check=True)
 e=ELFFile(io.BytesIO(obj.read_bytes()));assert not e.get_section_by_name('.rela.text');code=e.get_section_by_name('.text').data();u.mem_write(base,code)
 return {s.name:base+s['st_value']for s in e.get_section_by_name('.symtab').iter_symbols()},len(code)
cancel,cb=load('held_sound',0x4e080000);api,qb=load('held_queue',0x4e090000);CTX=0x4e006000
def init():
 reset();u.mem_write(CTX,bytes(32));u.mem_write(CTX+16,struct.pack('<Q',cancel['held_sound_cancel']))
def start(tone=1):
 call(0x40f9ce60,tone,0);m=ns['queued'].pop(0);assert call(api['held_queue_hold'],CTX,m)==1;return m
def stop(kind=32):
 assert call({31:0x40e31540,32:0x40e316bc}[kind])==1;return ns['queued'].pop(0)
def a(name,*args):return call(api['held_queue_'+name],CTX,*args)
rows=[]
# Native FIFO can place a cancel before a ready notice; cancellation consumes
# both heap envelopes and the later stable notice must do nothing.
for kind in (31,32):
 init();start();assert a('notice')==1 and a('notice')==0;s=stop(kind)
 assert a('cancel',s)==1;before=state();assert a('take',1)==0 and state()==before
 assert before['pending']==before['soundtask']==before['sfx']==0
 rows.append(dict(case='cancel before ready wake',kind=kind,final=before))
# Old control remains queued across cancellation and a new valid start. The
# same stable control is not reused; its old generation cannot release new030.
init();start();assert a('notice')==1;assert a('cancel',stop())==1
second=start(2);assert a('notice')==0 and a('take',1)==0
assert r64(CTX)==second and a('notice')==1
assert a('take',1)==second and r64(CTX)==0
s=stop();before=state();assert a('cancel',s)==0 and state()==before
rows.append(dict(case='stale wake generation then transfer',transferred=hex(second),cancel_delegates_native=True))
# Failed posting does not transfer ownership. No idle/permission => keep held.
init();m=start();assert a('notice')==1;a('post_failed');assert a('notice')==1
assert a('take',0)==0 and r64(CTX)==m;assert a('notice')==1
assert a('cancel',stop())==1 and a('take',1)==0
rows.append(dict(case='definitely rejected post retries; not-ready wake retains',final=state()))
# Once transfer wins on the original serialized task, software cancel cannot
# double-free it, nor may a new held transaction overwrite in-flight owner.
init();m=start();assert a('notice')==1 and a('take',1)==m
assert call(api['held_queue_hold'],CTX,0x4e123400)==0
assert a('cancel',stop(31))==0 and not ns['freed']
rows.append(dict(case='native transfer wins',no_software_completion=True))
report=dict(passed=True,production_ready=False,compiled_text_bytes={'cancel':cb,'queue':qb},cases=rows,
 sources={n:hashlib.sha256((P/n).read_bytes()).hexdigest()for n in ['held_queue.c','held_queue.h','held_sound.c','held_sound.h']},
 boundary='Actual C ARM ownership/generation code and original native callbacks execute. Wake admission/ready permission are controlled inputs. Actual G mailbox FIFO is tested separately. The hook, precise mic ownership predicate, normal playback/done adapter and control-envelope posting lifetime are not installed or claimed.')
(P/'held-queue.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS ARM held/control/cancel FIFO order and stale generation; no native hook installed')
