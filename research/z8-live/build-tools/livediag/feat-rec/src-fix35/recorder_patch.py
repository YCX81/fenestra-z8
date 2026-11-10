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
         ' add x9,sp,#528\n ldp x0,x1,[x9]\n ldp x2,x3,[x9,#16]\n mov x1,x19\n add x2,sp,#528\n'
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
void live_encoder_preempt_lamp(live_encoder *,uint32_t,struct fr_state *,const uint64_t *);
void live_w5_bringup_notify_lamp(uint32_t client,struct fr_state *session,const uint64_t *regs) {
 struct live_runtime *r=state();
 if(r){crumb(r,now_us(),EV_W5,client,LOAD(&r->stream.job));live_encoder_preempt_lamp(&r->stream.encoder,client,session,regs);}
 fr_checkpoint_value(session,regs,59u,0u);
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
  if(LOAD(&r->recorder.arm)) {
   fr_worker_tick(&r->recorder);
   if(photo && !LOAD(&r->shutdown) && !LOAD(&r->writer_active) && !LOAD(&r->stream.job)
     && LOAD(&r->capture.phase)==LA_READY)fr_prepare(r);
  }''');edits['runtime.c']=t
 # Restore exact v3b codec bodies before their recorder snapshot append.
 edits['codec/live_w5enc.c']=replace((SRC/'codec/live_w5enc.c').read_text(),'    EXIT(W5_CLIENT);','    fr_live_exit(W5_CLIENT);')
 edits['codec/live_w5enc.c']='#include \"../recorder/recorder.h\"\n'+edits['codec/live_w5enc.c']
 # Instrument a private copy reached ONLY by the already-armed assembly path.
 # D calls remain private; the shared close timeout gets a fixed session budget below.
 codec=edits['codec/live_w5enc.c']
 close=codec[codec.index('static int w5_close_body('):codec.index('/* v27: worker task, Live not encoding')]
 start=codec.index('void live_encoder_preempt(')
 end=codec.index('\n}',start)+2
 pre=codec[start:end]
 clone=(close+pre).replace('w5_close_body(', 'w5_close_body_lamp(').replace('w5_close(', 'w5_close_lamp(').replace('live_encoder_preempt(', 'live_encoder_preempt_lamp(')
 clone=clone.replace('struct live_w5 *w) {','struct live_w5 *w,struct fr_state *session,const uint64_t *regs) {')
 clone=clone.replace('live_encoder *e, uint32_t client) {','live_encoder *e, uint32_t client,struct fr_state *session,const uint64_t *regs) {')
 clone=clone.replace('w5_close_body_lamp(w)', 'w5_close_body_lamp(w,session,regs)').replace('w5_close_lamp(w)', 'w5_close_lamp(w,session,regs)')
 # v8: private MOVRS path only. All diagnostic points publish RAM.
 # Preserve original waits/conditions; wrap each existing DELAY exactly once.
 def point(stage,detail='0u'):
  return f'fr_checkpoint_value(session,regs,{stage}u,{detail});'
 # Close milestones in increasing order, including diag return and EXIT return.
 for native,stage in [(10,21),(11,23),(12,27),(13,29),(14,33),(15,35),(16,37),(17,39),(18,42),(19,46)]:
  import re
  pattern=rf'w5_diag\(w, {native}u, [^;]+;'
  clone=re.sub(pattern,lambda m:point(stage)+' '+m[0]+' '+point(stage+1),clone)
 # Each native blocking call has a distinct stage from the following wait.
 clone=replace(clone,'        if (SUBMIT(s, 0, list)) return 2;',
  '        '+point(20)+' if (SUBMIT(s, 0, list)) return 2;')
 clone=replace(clone,'    if (TRIGGER(s, 0, k, 0, 0)) return 4;',
  '    uint32_t trigger_rc=TRIGGER(s, 0, k, 0, 0); '+point(25)+' if (trigger_rc) return 4;')
 clone=replace(clone,'    if (TRIGGER(s, 0, (k + 1) % W5_SLOTS, 0, 0)) return 6;',
  '    trigger_rc=TRIGGER(s, 0, (k + 1) % W5_SLOTS, 0, 0); '+point(31)+' if (trigger_rc) return 6;')
 clone=replace(clone,'    fr_live_exit(W5_CLIENT);','    fr_live_exit(W5_CLIENT); '+point(41))
 clone=replace(clone,'while (VDI_BASE) { if (now_us() - t > 1000000u) return 9; DELAY(1); }',
  'while (VDI_BASE) { if (now_us() - t > 1000000u) { '+point(45,'1u')+' return 9; } DELAY(1); } '+point(45))
 clone=replace(clone,'    if (alive(w) && !w->close_failed) {','    fr_close_begin(session,regs,w); '+point(19)+'\n    if (alive(w) && !w->close_failed) {')
 clone=replace(clone,'    __atomic_store_n(&w->rmw->w5_close_lock, 0, __ATOMIC_RELEASE);','    __atomic_store_n(&w->rmw->w5_close_lock, 0, __ATOMIC_RELEASE);\n    fr_close_end(session,w); '+point(48))
 a=clone.index('void live_encoder_preempt_lamp(')
 close,pre=clone[:a],clone[a:]
 pre=replace(pre,'    w5_diag(w, 1u, client, (uint32_t)alive(w));',
  '    '+point(2)+' w5_diag(w, 1u, client, (uint32_t)alive(w)); '+point(3))
 pre=replace(pre,'    if (!alive(w)) return;',
  '    '+point(4)+' uint32_t active=(uint32_t)!!alive(w);\n    uint32_t busy0=!!__atomic_load_n(&w->busy,__ATOMIC_SEQ_CST);\n    fr_progress(session,regs,5u,(active<<1)|busy0,busy0,active,INT32_MIN);\n    if (!active) { '+point(58)+' return; }')
 pre=replace(pre,'    __atomic_store_n(&w->release_req, 1, __ATOMIC_SEQ_CST);',
  '    '+point(6)+' __atomic_store_n(&w->release_req, 1, __ATOMIC_SEQ_CST); '+point(7))
 pre=replace(pre,'    while (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST) && now_us() - start < 4000000u) DELAY(1);',
  '    '+point(8)+'\n    while (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST) && now_us() - start < 4000000u) {\n'
  '        fr_progress(session,regs,9u,1u,1u,active,INT32_MIN);\n'
  '        int32_t rc=DELAY(1);\n'
  '        uint32_t busy=!!__atomic_load_n(&w->busy,__ATOMIC_SEQ_CST);\n'
  '        fr_progress(session,regs,9u,busy|2u|((rc!=0)<<2),busy,active,rc);\n    }\n    '+point(10))
 pre=replace(pre,'if (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST)) { w->close_failed',
  'if (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST)) { '+point(11,'1u')+' w->close_failed')
 pre=replace(pre,'else { w5_diag(w, 2u, w->busy, 0); if (alive(w)) w5_close_lamp(w,session,regs); }',
  'else { '+point(12)+' w5_diag(w, 2u, w->busy, 0); '+point(13)+' if (alive(w)) { '+point(14)+' w5_close_lamp(w,session,regs); } }')
 pre=replace(pre,'    w5_diag(w, 30u, w->fin_rc, w->close_failed);','    '+point(50)+' w5_diag(w, 30u, w->fin_rc, w->close_failed); '+point(51))
 pre=replace(pre,'    __atomic_store_n(&w->release_req, 0, __ATOMIC_SEQ_CST);','    '+point(52)+' __atomic_store_n(&w->release_req, 0, __ATOMIC_SEQ_CST); '+point(53))
 # Existing close DELAYs: lock wait, EOS output, drain, VDI. No added sleep.
 # Both sides use one major stage; detail 0=before, 2=returned, 6=error.
 # Locate by surrounding loops rather than replacing the native function globally.
 close=close.replace('DELAY(1);','FR_DELAY_PLACEHOLDER;')
 stages=[26,32,44,16] # source order: body waits then close_lock wait
 assert close.count('FR_DELAY_PLACEHOLDER;')==len(stages)
 for stage in stages:
  close=close.replace('FR_DELAY_PLACEHOLDER;',
   '{ '+point(stage)+' int32_t rc=DELAY(1); fr_progress(session,regs,'+str(stage)+'u,2u|((rc!=0)<<2),!!w->busy,0xffffffffu,rc); }',1)
 close=close.replace('if (now_us() - t > 500000u) return 5;', 'if (now_us() - t > 500000u) { '+point(26,'4u')+' return 5; }')
 close=replace(close,'if (now_us() - t > 1000000u) return 7;', 'if (now_us() - t > 1000000u) { '+point(32,'4u')+' return 7; }')
 # v9 fills previously unused stages without changing v8b meanings or hooks.
 close=replace(close,'    while (!__atomic_compare_exchange_n(&w->rmw->w5_close_lock',
  '    '+point(15)+'\n    while (!__atomic_compare_exchange_n(&w->rmw->w5_close_lock')
 close=replace(close,'    fr_close_begin(session,regs,w);',
  '    '+point(17)+' fr_close_begin(session,regs,w);')
 close=replace(close,'if (SUBMIT(s, 0, list)) return 2;',
  'uint32_t submit_rc=SUBMIT(s, 0, list); '+point(20,'1u')+' if (submit_rc) return 2;')
 close=replace(close,'    if (W5_STOP(s)) return 3;',
  '    uint32_t stop_rc=W5_STOP(s); '+point(22,'1u')+' if (stop_rc) return 3;')
 pre=replace(pre,'w5_close_lamp(w,session,regs); }',
  'w5_close_lamp(w,session,regs); '+point(49)+' }')
 pre=replace(pre,point(53),point(53)+' '+point(54))
 clone=close+pre
 # ALL competitors sample the armed/claimed session once, before waiting.
 def grace(text):
  text=replace(text,'uint32_t free_ = 0; uint64_t t = now_us();',
    'uint32_t free_ = 0; uint64_t t = now_us();\n    const uint64_t budget = 4000000u + fr_close_grace();')
  return replace(text,'        if (now_us() - t > 4000000u)',
    '        const uint64_t elapsed = now_us() - t;\n        if (elapsed > budget)')
 clone=clone.replace('w->fin_rc = (w->fin_rc & 0xffffffu) | 11u << 24;',point(18,'1u')+' w->fin_rc = (w->fin_rc & 0xffffffu) | 11u << 24;')
 edits['codec/live_w5enc.c']=grace(codec)+'\n/* v6: private armed REC diagnostics; shared close has a fixed deadline. */\n'+grace(clone)+'\n'
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
 manifest={'diagnostic':'lamp-v9b','experiment':{'name':'skip_nr','default':0,'prefix_sha256':NR_PREFIX_SHA,'stock_exit_patched':False},'reference_v3b_sha256':'6daae383c5b81cc55c97c76cb79eeb953a51eaa911ea0d3bf86ed5f7e087033e','base_sha256':SHA,'base':hex(BASE),'payload_start':'0x401e2000','payload_limit':'0x401fff80',
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
 layout.write_text('#include "runtime.h"\n#include <stddef.h>\nconst unsigned fr_layout[]={sizeof(struct live_runtime),offsetof(struct live_runtime,recorder),sizeof(struct fr_state),offsetof(struct fr_state,tx),offsetof(struct live_runtime,card_epoch),offsetof(struct live_runtime,shutdown),FR_BYTES,FR_SLOTS,offsetof(struct fr_state,skip_nr),offsetof(struct fr_state,live_exit_active),0x4e523031,0x4c503036};\n')
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
