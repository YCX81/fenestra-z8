#!/usr/bin/env python3
"""v35 source overlay + guarded relocation manifest. Never accesses a device.
Default: create ../recorder-build from this v35 tree; --check compiles touched units.
No updater or flash image is generated. relive integration: see recorder/README.md.
"""
import argparse, hashlib, json, shutil, struct, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
SRC=Path(__file__).resolve().parent
SHA='c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4'
BASE=0x40160000
NR_PREFIX_SHA='ae69d151058541eb02db2596a60d903e0bcca6ce9c12ae74f8730039a8abeb04'
NR_DEFS={'fr_native_exit':0x402f3084,'fr_native_tail':0x402f3194,'fr_native_reset':0x40c2d884}
NR_DEFS.update({f'fr_native_{a:x}':a for a in (0x402f2b94,0x40c3325c,0x40ca1990,0x402f2bac,0x40c001f4,0x40c2d0f8,0x40c2e7fc,0x40c2fb7c,0x40c2d884)})
# event, site, expected LE bytes, relocation kind, original target
PROBES=[(2,0x402f303c,'00010034','cbz',0x402f305c),
 (3,0x402f3068,'f37b47a9','plain',0)]
def extend_relive(definitions, sites):
 """Call before the existing relive link/patch steps (int->(symbol,call) sites).
 Rebuild ALL v35 objects because runtime.h has appended state. Do not call build.py.
 """
 if sites.get(0x402f2d58) != ('live_w5_bringup_entry',False):
  raise ValueError('relive must declare the existing bring-up B hook')
 if definitions.get('live_w5_bringup_resume') != 0x402f2d5c:
  raise ValueError('unexpected existing bring-up continuation')
 for ev,at,raw,kind,target in PROBES:
  if at in sites:raise ValueError(f'probe overlaps existing hook: {at:x}')
  sites[at]=(f'fr_entry_{ev}',False)
  definitions[f'fr_next_{ev}']=at+4
  if target:definitions[f'fr_target_{ev}']=target
 definitions.update(NR_DEFS)
 return definitions,sites

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def inside(p):
 p=p.resolve()
 if not p.is_relative_to(ROOT):raise ValueError('output must stay inside feat-rec')
 return p
def replace(text,old,new):
 if text.count(old)!=1:raise ValueError('v35 anchor mismatch: '+old[:100])
 return text.replace(old,new)
def macros():
 s='.arch armv8-a+simd\n.macro fr_save\n save_all\n sub sp,sp,#528\n'
 for i in range(0,32,2):s+=f' stp q{i},q{i+1},[sp,#{i*16}]\n'
 s+=' mrs x9,fpcr\n mrs x10,fpsr\n str x9,[sp,#512]\n str x10,[sp,#520]\n.endm\n.macro fr_restore\n ldr x9,[sp,#512]\n ldr x10,[sp,#520]\n msr fpcr,x9\n msr fpsr,x10\n'
 for i in range(0,32,2):s+=f' ldp q{i},q{i+1},[sp,#{i*16}]\n'
 return s+' add sp,sp,#528\n restore_all\n.endm\n'
def arm_gate(reject):
 # Fixed UI ABI from ui/live_ui.c and ui/fx_registry.h. Runtime layout is
 # compile-asserted below. Only x9-x12, NZCV and a 32-byte scratch frame.
 # Every dynamic dereference is preceded by alignment + RAM extent checks.
 s=' stp x9,x10,[sp,#-32]!\n stp x11,x12,[sp,#16]\n mrs x11,nzcv\n'
 def ram(n):
  return (f' lsr x10,x9,#30\n cmp x10,#40\n b.ne 80f\n'
          f' tst x9,#7\n b.ne 80f\n mov x10,#{n-1}\n add x10,x9,x10\n'
          ' lsr x10,x10,#30\n cmp x10,#40\n b.ne 80f\n')
 def identity(offset,value,half=False):
  return (f' {"ldrh" if half else "ldr"} w10,[x9,#{offset}]\n'
          f' mov w12,#{value&65535}\n movk w12,#{value>>16},lsl #16\n'
          ' cmp w10,w12\n b.ne 80f\n')
 s+=' mov x9,#0x50d8\n movk x9,#0x032b,lsl #16\n movk x9,#0xa,lsl #32\n ldar x9,[x9]\n'
 s+=ram(664*96)+' mov x10,#63728\n add x9,x9,x10\n ldar x9,[x9]\n'
 s+=ram(2536)+identity(64,0x465a394d)
 s+=' ldr w10,[x9,#68]\n cbz w10,80f\n'+identity(32,46,True)
 s+=' add x9,x9,#2528\n ldar x9,[x9]\n'
 s+=ram(64)+identity(0,0x47525846)+identity(4,64)
 s+=' add x9,x9,#8\n ldar x9,[x9]\n'
 s+=ram(16)+identity(0,0x4c505549)
 s+=' add x9,x9,#8\n ldar x9,[x9]\n'
 s+=ram(26512)+identity(0,0x4c415231)+identity(4,26512)
 s+=' mov x10,#0x5a90\n add x9,x9,x10\n ldar w10,[x9]\n cbz w10,80f\n'
 restore=' msr nzcv,x11\n ldp x11,x12,[sp,#16]\n ldp x9,x10,[sp],#32\n'
 return s+restore+' b 81f\n80:\n'+restore+f' b {reject}\n81:\n'
def bringup():
 # X19 pins the accepted session across ABI-conforming C calls. The original
 # X19 remains in the 784-byte frame. I precedes argument/NZCV restoration;
 # R is the first diagnostic after notify, with no eligibility/file callback.
 return ('live_w5_bringup_entry:\n'+arm_gate('93f')+
         ' fr_save\n add x0,sp,#528\n bl fr_before\n mov x19,x0\n'
         '.global fr_lamp_i\nfr_lamp_i:\n mov w1,#0\n add x2,sp,#528\n bl fr_light\n'
         ' add x9,sp,#528\n ldp x0,x1,[x9]\n ldp x2,x3,[x9,#16]\n ldp x4,x5,[x9,#32]\n ldp x6,x7,[x9,#48]\n'
         ' ldr x9,[sp,#776]\n msr nzcv,x9\n'
         '.global fr_notify_call\nfr_notify_call:\n bl live_w5_bringup_notify_lamp\n'
         '.global fr_lamp_r\nfr_lamp_r:\n mov x0,x19\n mov w1,#1\n add x2,sp,#528\n bl fr_light\n'
         ' fr_restore\n b 92f\n93:\n.global fr_bringup_unarmed\nfr_bringup_unarmed:\n save_all\n bl live_w5_bringup_notify\n restore_all\n92:\n.global fr_stock_1\nfr_stock_1:')
def nr_hooks(image):
 # Relocate ONLY the straight stock prefix, retaining its 32-byte frame and
 # native tail at 3194. Fail closed for every PC-relative opcode not handled.
 from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
 d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
 raw=image[0x402f3084-BASE:0x402f3194-BASE]
 assert hashlib.sha256(raw).hexdigest()==NR_PREFIX_SHA
 s = """
.global fr_exit_scoped
fr_exit_scoped:
 stp x22,x30,[sp,#-32]!
 mov x22,x0
 sub x1,sp,#32
 bl fr_nr_begin
 cbz w0,1f
 mov w0,#4
 bl fr_exit_clone
.global fr_exit_return
fr_exit_return:
 str x0,[sp,#16]
 mov x0,x22
 bl fr_nr_end
 ldr x0,[sp,#16]
 ldp x22,x30,[sp],#32
 ret
1:
 mov w0,#4
 ldp x22,x30,[sp],#32
 b fr_native_exit
.global fr_exit_clone
fr_exit_clone:
"""
 for i in d.disasm(raw,0x402f3084):
  s+=f'.global fr_clone_{i.address:x}\nfr_clone_{i.address:x}:\n.Lclone_{i.address:x}:\n'
  if i.address in (0x402f3184,0x402f3190):
   suffix='a' if i.address==0x402f3184 else 'b'
   s+=f' fr_save\n mov x0,x22\n add x1,sp,#528\n adr x2,.Lnr_lr_{suffix}\n bl fr_nr_allow\n cbz w0,1f\n fr_restore\n b 2f\n1:\n fr_restore\n bl fr_native_reset\n2:\n'
  elif i.mnemonic=='bl':
   target=int(i.op_str[1:],16);s+=f' bl fr_native_{target:x}\n'
  elif i.mnemonic=='cbz':
   assert i.address==0x402f3098
   s+=' cbz w0,.Lclone_402f30b0\n'
  else:
   assert not (i.mnemonic.startswith('b') or i.mnemonic in ('adr','adrp','ldr')) or (i.mnemonic=='ldr' and '[' in i.op_str)
   s+=f' .inst 0x{int.from_bytes(i.bytes,"little"):08x}\n'
  if i.address==0x402f3178:s+='.global fr_nr_lr_a\nfr_nr_lr_a:\n.Lnr_lr_a:\n'
  # Both branches restore the saved frame; normalize the first call's LR to
  # its common continuation so the second guard has one exact expected LR.
  if i.address==0x402f3184:
   s+=' adr x30,.Lnr_lr_b\n.global fr_nr_lr_b\nfr_nr_lr_b:\n.Lnr_lr_b:\n'
 s+=' b fr_native_tail\n'
 return s

def hooks():
 s='\n/* Lamp v4: B trampolines; all C runs inside the full 784-byte frame. */\n'
 for ev,site,raw,kind,target in PROBES:
  s+=f'.global fr_entry_{ev}\nfr_entry_{ev}:\n'+arm_gate('92f')+f' fr_save\n mov w0,#{ev}\n add x1,sp,#528\n bl fr_probe\n fr_restore\n92:\n.global fr_stock_{ev}\nfr_stock_{ev}:\n'
  if kind=='plain':s+=f' .inst 0x{int.from_bytes(bytes.fromhex(raw),"little"):08x}\n b fr_next_{ev}\n'
  elif kind=='cbz':s+=f' cbnz w0,1f\n b fr_target_{ev}\n1: b fr_next_{ev}\n'
  else:raise ValueError('unreviewed displaced instruction')
 return s
RUNTIME=r'''/* Recorder: state is zeroed at boot; arm is written only by the owner. */
#include <stddef.h>
_Static_assert(offsetof(struct live_runtime,recorder)==0x5a90,"arm gate offset");
_Static_assert(sizeof(struct live_runtime)==26512,"arm gate runtime extent");
_Static_assert(offsetof(struct fr_state,arm)==0,"arm gate field");
struct fr_state *fr_context(void) {
 struct live_runtime *r=state();if(!r || !LOAD(&r->recorder.arm))return 0;
 return &r->recorder;
}
void live_w5_bringup_notify(uint32_t client) {
 struct live_runtime *r=state();
 if(r){crumb(r,now_us(),EV_W5,client,LOAD(&r->stream.job));live_encoder_preempt(&r->stream.encoder,client);}
 }
void live_encoder_preempt_lamp(live_encoder *,uint32_t);
void live_w5_bringup_notify_lamp(uint32_t client) {
 struct live_runtime *r=state();
 if(r){crumb(r,now_us(),EV_W5,client,LOAD(&r->stream.job));live_encoder_preempt_lamp(&r->stream.encoder,client);}
}
'''
def apply(out,image):
 b=image.read_bytes()
 if hashlib.sha256(b).hexdigest()!=SHA:raise ValueError('bg34 MAIN hash mismatch')
 for _,at,raw,_,_ in PROBES:
  if b[at-BASE:at-BASE+4]!=bytes.fromhex(raw):raise ValueError(f'wrong instruction {at:x}')
 if b[0x402f2d58-BASE:0x402f2d5c-BASE].hex()!='0fc9fb17':raise ValueError('bringup hook mismatch')
 if out.exists():raise ValueError('output exists; choose a new local folder (no overwrite)')
 # Validate text anchors before any source mutation. Never touch the input tree.
 edits={}
 h=(SRC/'runtime.h').read_text();h=replace(h,'#include <stdint.h>','#include <stdint.h>\n#include "recorder/recorder.h"')
 h=replace(h,'struct live_runtime {','struct live_runtime {') # version anchor
 h=replace(h,'void live_irq_notify','void live_irq_notify')
 marker=' uint32_t vhead,vpad;struct live_ev vring[64]; /* v23: view/playback changes, RAM only (no card saves) */'
 h=replace(h,marker,marker+'\n struct fr_state recorder; /* appended; zeroed at boot */');edits['runtime.h']=h
 t=(SRC/'runtime.c').read_text();a=t.index('/* v35: card lamp');z=t.index('/* DMA callback context',a)
 t=t[:a]+RUNTIME+t[z:]
 t=replace(t,'  if(r->stream.encoder.hw)live_encoder_diag_tick(&r->stream.encoder); /* v33 */',
 '''  /* Explicit RAM arm required BEFORE any recorder prepare call. */
  if(LOAD(&r->recorder.arm) && photo && !LOAD(&r->shutdown) && !LOAD(&r->writer_active) && !LOAD(&r->stream.job)
     && LOAD(&r->capture.phase)==LA_READY)fr_prepare(r);''');edits['runtime.c']=t
 # Restore exact v3b codec bodies before their recorder snapshot append.
 edits['codec/live_w5enc.c']=replace((SRC/'codec/live_w5enc.c').read_text(),'    EXIT(W5_CLIENT);','    fr_live_exit(W5_CLIENT);')
 edits['codec/live_w5enc.c']='#include \"../recorder/recorder.h\"\n'+edits['codec/live_w5enc.c']
 # Instrument a private copy reached ONLY by the already-armed assembly path.
 # Original worker and unarmed preempt/close bodies gain no guards/calls.
 codec=edits['codec/live_w5enc.c']
 close=codec[codec.index('static int w5_close_body('):codec.index('/* v27: worker task, Live not encoding')]
 pre=codec[codec.index('void live_encoder_preempt('):codec.index('void live_encoder_preempt(')] if False else ''
 start=codec.index('void live_encoder_preempt(')
 end=codec.index('\n}',start)+2
 pre=codec[start:end]
 clone=(close+pre).replace('w5_close_body(', 'w5_close_body_lamp(').replace('w5_close(', 'w5_close_lamp(').replace('live_encoder_preempt(', 'live_encoder_preempt_lamp(')
 import re
 clone=re.sub(r'w5_diag\(w, (\d+)u,',r'fr_checkpoint(\1u); w5_diag(w, \1u,',clone)
 edits['codec/live_w5enc.c']=codec+'\n/* v6: private armed REC diagnostics; original close untouched. */\n'+clone+'\n'
 edits['codec/live_codec.h']=(SRC/'codec/live_codec.h').read_text()
 t=(SRC/'hooks.S').read_text();t=replace(t,'.global live_irq_entry',macros()+'.global live_irq_entry')
 t=replace(t,'live_w5_bringup_entry:\n save_all\n bl live_w5_bringup_notify\n restore_all',
 bringup())
 edits['hooks.S']=t+hooks()+nr_hooks(b)
 shutil.copytree(SRC,out,ignore=shutil.ignore_patterns('__pycache__','._*'))
 for name,text in edits.items():(out/name).write_text(text)
 defs={'live_w5_bringup_resume':0x402f2d5c,**NR_DEFS};sites={}
 for ev,at,raw,kind,target in PROBES:
  defs[f'fr_next_{ev}']=at+4
  if target:defs[f'fr_target_{ev}']=target
  sites[hex(at)]={'symbol':f'fr_entry_{ev}','call':False,'expected':raw,'event':ev,'kind':kind}
 (out/'recorder-defs.ld').write_text('\n'.join(f'{k} = {v:#x};' for k,v in defs.items())+'\n')
 manifest={'diagnostic':'lamp-v6','experiment':{'name':'skip_nr','default':0,'prefix_sha256':NR_PREFIX_SHA,'stock_exit_patched':False},'reference_v3b_sha256':'6daae383c5b81cc55c97c76cb79eeb953a51eaa911ea0d3bf86ed5f7e087033e','base_sha256':SHA,'base':hex(BASE),'payload_start':'0x401e2000','payload_limit':'0x401fff80',
 'add_objects':['recorder/recorder.c'],'sites':sites,'definitions':defs,'source_sha256':{n:digest(SRC/n) for n in edits},
 'modified_sha256':{n:digest(out/n) for n in edits},'full_relink_required':True,'hardware_tested':False,
 'preserved_core_helpers':['0x401fe000','0x401fff80'],
 'recorder_sha256':{p.name:digest(p) for p in (SRC/'recorder').iterdir() if p.is_file()}}
 (out/'recorder-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 # Reviewable source delta; no dependence on git worktree.
 import difflib
 (out/'recorder-hooks.patch').write_text(''.join(difflib.unified_diff((SRC/'hooks.S').read_text().splitlines(True),edits['hooks.S'].splitlines(True),fromfile='v35/hooks.S',tofile='recorder/hooks.S')))
 return manifest

def check(out,cc):
 obj=out/'recorder-check';obj.mkdir(exist_ok=True)
 commands=[]
 for name in ['runtime.c','codec/live_w5enc.c','recorder/recorder.c','hooks.S']:
  cmd=[cc,'--target=aarch64-none-elf','-march=armv8-a','-Os','-ffreestanding','-fno-builtin','-fstack-usage','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-mgeneral-regs-only','-mstrict-align','-Wall','-Wextra','-Werror','-I',str(out),'-I',str(out/'ui/core/include'),'-I',str(out/'codec/freestanding'),'-c',str(out/name),'-o',str(obj/(name.replace('/','_')+'.o'))]
  if name.endswith(".S"):cmd.remove("-mgeneral-regs-only")
  subprocess.run(cmd,check=True);commands.append(cmd)
 layout=out/'recorder-layout.c'
 layout.write_text('#include "runtime.h"\n#include <stddef.h>\nconst unsigned fr_layout[]={sizeof(struct live_runtime),offsetof(struct live_runtime,recorder),sizeof(struct fr_state),offsetof(struct fr_state,tx),offsetof(struct live_runtime,card_epoch),offsetof(struct live_runtime,shutdown),FR_BYTES,FR_SLOTS,offsetof(struct fr_state,skip_nr),offsetof(struct fr_state,live_exit_active),0x4e523031};\n')
 cmd=[cc,'--target=aarch64-none-elf','-I',str(out),'-I',str(out/'ui/core/include'),'-c',str(layout),'-o',str(obj/'layout.o')]
 subprocess.run(cmd,check=True);commands.append(cmd)
 (obj/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 print('PASS: four modified AArch64 units compile; not a full relive link or hardware test')
if __name__=='__main__':
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,default=ROOT/'recorder-build');ap.add_argument('--main',type=Path,default=ROOT/'bg34.main');ap.add_argument('--check',action='store_true');ap.add_argument('--cc',default='clang');a=ap.parse_args()
 out=inside(a.out)
 if out==SRC or out.is_relative_to(SRC):ap.error('output must be outside input src-fix35')
 apply(out,a.main)
 if a.check:check(out,a.cc)
 print(out)
