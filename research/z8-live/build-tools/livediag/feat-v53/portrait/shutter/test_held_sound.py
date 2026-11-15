#!/usr/bin/env python3
"""Actual compiled ARM private helper + original native completion callbacks.
It is an uninstalled dispatcher-thread prototype, not a full sound hook.
"""
from pathlib import Path
import hashlib,io,json,struct,subprocess
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import *
P=Path(__file__).resolve().parent
ns={'__file__':str(P/'test_sound_cancel.py')}
exec(compile((P/'test_sound_cancel.py').read_text().split('rows=[]')[0],str(P/'test_sound_cancel.py'),'exec'),ns)
u=ns['u'];call=ns['call'];state=ns['state'];reset=ns['reset'];w32=ns['w32'];r64=ns['r64']
obj=P/'held_sound.o'
subprocess.run(['clang','--target=aarch64-none-elf','-std=c11','-O2','-fno-stack-protector','-fno-builtin','-c',str(P/'held_sound.c'),'-o',str(obj)],check=True)
e=ELFFile(io.BytesIO(obj.read_bytes()));assert not e.get_section_by_name('.rela.text')
code=e.get_section_by_name('.text').data();BASE=0x4e080000;u.mem_write(BASE,code)
entry=BASE+next(s['st_value']for s in e.get_section_by_name('.symtab').iter_symbols()if s.name=='held_sound_cancel')
CTX=0x4e006000
def fixture(tone,cancel):
 reset();call(0x40f9ce60,tone,0);start=ns['queued'].pop(0)
 assert call({31:0x40e31540,32:0x40e316bc}[cancel])==1
 stop=ns['queued'].pop(0);u.mem_write(CTX,struct.pack('<QII',start,1,17));return start,stop
rows=[]
for tone in (1,2):
 for cancel in (31,32):
  start,stop=fixture(tone,cancel);before=state();rc=call(entry,CTX,stop);after=state()
  assert rc==1 and after['pending']==after['soundtask']==after['sfx']==0
  assert after['allocated']==after['freed']==5 and r64(CTX)==0
  callbacks=[r for r in ns['trace'] if r['pc']=='0x40f9d194']
  assert [r['event']for r in callbacks]==['0x4102004','0x4102005']
  assert all(r['words']==[tone,0]for r in callbacks)
  count=len(ns['trace']);assert call(entry,CTX,stop)==0 and len(ns['trace'])==count
  rows.append(dict(case='software cancel before native transfer',tone=tone,cancel=cancel,before=before,after=after,trace=ns['trace'][:],duplicate_noop=True))
# A no-owner/transfer/mismatch must not dereference/free native envelopes.
for kind in ('empty','claimed','transferred','active_sfx','foreign_deferred','other_tone','loop','other_callback','other_envelope','bad_cancel'):
 start,stop=fixture(1,32)
 if kind in ('empty','claimed','transferred'):w32(CTX+8,{'empty':0,'claimed':2,'transferred':4}[kind])
 elif kind=='active_sfx':w32(0xa038feb74,1)
 elif kind=='foreign_deferred':u.mem_write(0xa038febb8,struct.pack('<Q',0x40f9d194))
 elif kind=='other_tone':w32(r64(r64(start+40))+12,12)
 elif kind=='loop':w32(r64(r64(start+40))+8,0)
 elif kind=='other_callback':u.mem_write(r64(r64(start+40))+24,struct.pack('<Q',0x40f9d195))
 elif kind=='other_envelope':u.mem_write(start+48,struct.pack('<Q',start+8))
 elif kind=='bad_cancel':w32(stop+32,0x04100030)
 before=state();rc=call(entry,CTX,stop);after=state();assert rc in (0,0xffffffff) and before==after and not ns['freed'] and not ns['trace']
 rows.append(dict(case=kind,rc=hex(rc),unchanged=True))
report=dict(passed=True,production_ready=False,compiled_text_bytes=len(code),source_sha256=hashlib.sha256((P/'held_sound.c').read_bytes()).hexdigest(),cases=rows,
 prerequisites=['Only original serialized SFX dispatcher may call this; no concurrent native SFX state mutation.',
 'Context state1 guarantees held030 never submitted/reposted to original native queue; claimed or transferred states reject.',
 'Only normal tone1/2 repeat1 and exact original callbacks/envelope shapes are accepted.',
 'Caller must separately prove enqueue-publication, STOP/handoff ordering, stop/start microphone timing and replay admission.'],
 boundary='Actual compiled helper executes in ARM emulator against exact delivered-G callbacks/free wrappers/getters. Heap, OS events, passive logs and duration are instrumented. No production hook, real task concurrency or hardware sound has been tested.')
(P/'held-sound.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS',len(rows),'ARM helper/native callbacks; private software-cancel protocol, no installed hook')
