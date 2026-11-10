#!/usr/bin/env python3
"""Fail-closed final ELF/MAIN audit. Read-only; needs pyelftools. Not hardware acceptance."""
import argparse, hashlib, importlib.util, io, json, struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
BASE, START, LIMIT = 0x40160000, 0x401e2000, 0x401fff80
PINNED = 0x401fe000  # base core helpers, not part of the Live replacement
OLD = {0x403db694:('live_ui_menu_page',False),0x4032e130:('live_ui_menu_text',False),
 0x40d60ec8:('live_ui_system_dispatch',False),0x40fdc9c8:('live_press_entry',False),
 0x40e90838:('live_irq_entry',False),0x40c26804:('live_start_entry',False),
 0x40c26818:('live_stop_entry',False),0x40c26828:('live_hold_entry',False),
 0x40e8b328:('live_pool_wrap',True),0x408b7c1c:('live_closed_entry',False),
 0x406cd6a0:('live_finalize_wrap',True),0x406cd75c:('live_event_wrap',True),
 0x40ea3638:('live_writer_wrap',True),0x406cd468:('live_open_wrap',True),
 0x402f2d58:('live_w5_bringup_entry',False)}
# Independent bg34 evidence: (baseline branch PC, stock destination).
# These PCs are in the SHA-pinned baseline, NEVER taken from the candidate ELF.
BASE_TRANSFERS = {
 'live_irq_resume':(0x401e4dec,0x40e9083c),
 'live_start_resume':(0x401e4f20,0x40c26808),
 'live_stop_resume':(0x401e4fbc,0x40c2681c),
 'live_hold_resume':(0x401e5058,0x40c2682c),
 'live_closed_resume':(0x401e5190,0x408b7c20),
 'live_w5_bringup_resume':(0x401e522c,0x402f2d5c),
 'live_old_movie_press':(0x401e4e84,0x40a020c4),
 'live_old_pool':(0x401e50f0,0x4104356c),
 'live_old_menu_page':(0x401e680c,0x401cce84),
 'live_old_menu_text':(0x401e689c,0x401cd3ec),
 'live_old_system_dispatch':(0x401e6a40,0x401cd788),
 'live_old_writer':(0x401e42e4,0x409f262c),
}
OLD_DEFS = {n:v for n,(_,v) in BASE_TRANSFERS.items()}
# The remaining three old hooks call absolute constants in runtime.c (FN),
# not linker symbols: open at line 273, finalize 341, event 344 in v35.
FORWARDS = {
 'live_ui_menu_page':0x401cce84,'live_ui_menu_text':0x401cd3ec,
 'live_ui_system_dispatch':0x401cd788,'live_press_entry':0x40a020c4,
 'live_pool_wrap':0x4104356c,'live_writer_wrap':0x409f262c,
 'live_open_wrap':0x4085b320,'live_finalize_wrap':0x4085b6d4,
 'live_event_wrap':0x406bd954,
}
# The proof starts at a fixed patch PC, not at an ELF symbol. These limits
# deliberately do NOT grow with candidate st_value/st_size/section boundaries.
PATH_LIMIT = 256
STATE_LIMIT = 8192

# Source-derived read-only lookup contract, independently recompiled with Apple
# clang 21 and byte-identical to the laboratory clang 21.1.8 objects. Source:
# ui/live_ui.c SHA256 0c97eaaf661ed3b52c53ab4233d7289470dd0e06aa7e790df914a85757ae523d.
# These are NOT trusted candidate symbols or arbitrary accepted return sites.
# Every byte is checked; only UI's single BL relocation varies. Both helpers
# are also walked normally, including every conditional and stack return.
PAGE_UI = bytes.fromhex('fd7bbfa9fd03009100000094e00400b408802791e0031faa08fddfc81f0940f241040054e99f62b2ea9f62b2a9fedff20901098b3f010aeb83030054090140b9ca088b524aeaa8723f010a6bc1020054090540b93f0101716102005408210091c9fedfd20af9965208fddfc8e9fffff2eaffa772e0031faa0901098b3f010aeb4801005409094092090100b5090140b92aa98a520a8aa9723f010a6b00019f9a02000014e0031faafd7bc1a8c0035fd6')
PAGE_CORE = bytes.fromhex('081b8ad209e080920ae080926865a0f20900b8f20a00b8f24801c0f2a9fedff2080140f90901098b3f010aeb83030054091e9f52cafedfd20801098beafffff208fddfc8090940920a010a8b3f0100f1090894d28951a0f24901c0f2040149fa09c39e52e9ffa772421149fa88010054094140b9aa2987524acba8723f010a6be1000054094540b9a9000034094140793fb9007100019f9ac0035fd6e0031faac0035fd6')

def prove_forward(read, entry, target, low=START, high=PINNED, page_contract=False):
 """Conservative all-path A64 proof up to the first stock transfer.

 Unknown data is abstracted, both conditional successors must succeed. Calls
 are followed (never assumed to return); RET is allowed only for a proved
 call/return edge. A wrapper return before stock fails except the complete source-derived page
 contract below. Loops, unsupported instructions and escapes still fail. This is a control-flow proof under normal ABI stack/memory
 execution, not a hardware termination/fault proof. No ELF metadata is consumed.
 """
 from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
 from capstone.arm64 import ARM64_OP_REG, ARM64_OP_IMM, ARM64_OP_MEM
 d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN);d.detail=True
 # No arbitrary stores, system instructions, exceptions, authenticated branches,
 # or opaque calls: such paths need a separately reviewed model, not a PASS.
 plain={'nop','mov','movk','add','sub','cmp','cmn','tst','and','orr','eor',
        'ccmp','lsl','lsr','asr','ubfx','sxtw','uxtb','uxth','csel','cset',
        'ldr','ldp','ldur','ldrb','ldrh','ldrsw','ldar','mrs','msr','str','stp'}
 cache={};active=set();count=0
 # A source-derived *complete entry shape*, not a symbol exemption. All 14
 # instructions, both guards, the output expression, stack restore and old
 # destination are fixed. The callee is still executed by this proof engine.
 # ui/live_ui.c: group==2 && id==0x705e -> u ? &u->child.page : NULL.
 # Any other input must tail-transfer to the independently pinned old target.
 page_return=None
 if page_contract:
  raw=read(entry,56) if read(entry,4)==struct.pack('<I',0x7100081f) else b''
  words=struct.unpack('<14I',raw) if len(raw)==56 else ()
  fixed={0:0x7100081f,1:0x54000181,2:0x528e0bc8,3:0x6b08003f,
         4:0x54000121,5:0xa9bf7bfd,6:0x910003fd,8:0x9100e008,
         9:0xf100001f,10:0x9a8803e0,11:0xa8c17bfd,12:0xd65f03c0}
  if words and all(words[k]==v for k,v in fixed.items()) and words[7]>>26==37 and words[13]>>26==5:
   dest=entry+52+(((words[13]&0x3ffffff)^0x2000000)-0x2000000)*4
   if dest==target:
    helper=entry+28+(((words[7]&0x3ffffff)^0x2000000)-0x2000000)*4
    ui=read(helper,len(PAGE_UI))
    if ui[:8]==PAGE_UI[:8] and ui[12:]==PAGE_UI[12:]:
     call=struct.unpack_from('<I',ui,8)[0]
     core=helper+8+(((call&0x3ffffff)^0x2000000)-0x2000000)*4
     if call>>26==37 and read(core,len(PAGE_CORE))==PAGE_CORE:page_return=entry+48

 def key(i,r):
  n=i.reg_name(r)
  return {'fp':'x29','lr':'x30','wsp':'sp','wzr':'xzr'}.get(n,
          'x'+n[1:] if n.startswith('w') else n)
 def walk(pc,values,returns,stack,depth):
  nonlocal count
  assert low<=pc<high and pc%4==0,('forward path escaped',hex(pc))
  assert depth<PATH_LIMIT,('forward path limit',hex(pc))
  state=(pc,tuple(sorted(values.items())),returns,tuple(sorted(stack.items())))
  assert state not in active,('forward path cycle',hex(pc))
  if state in cache:return
  count+=1;assert count<=STATE_LIMIT,'forward state limit'
  active.add(state)
  raw=read(pc,4);ins=list(d.disasm(raw,pc,count=1))
  assert len(raw)==4 and len(ins)==1 and ins[0].size==4,('invalid instruction',hex(pc))
  i=ins[0];ops=i.operands;mn=i.mnemonic
  # MOV also names SIMD lane insert/extract instructions. Never propagate
  # one lane as if it were the whole vector (or a different lane).
  gpr={'x'+str(r) for r in range(31)}|{'sp','xzr'}
  assert all(key(i,o.reg) in gpr and o.vector_index==-1
             for o in ops if o.type==ARM64_OP_REG),('unsupported register/lane',hex(pc))
  def width(r):return 4 if i.reg_name(r).startswith('w') else 8
  def value(r):
   v=0 if key(i,r)=='xzr' else values.get(key(i,r))
   return (v&0xffffffff if isinstance(v,int) else None) if width(r)==4 else v
  def next_pc(dest,v=values,rs=returns,st=stack):walk(dest,v,rs,st,depth+1)
  if mn in ('b','bl','br','blr'):
   dest=ops[0].imm if mn in ('b','bl') else value(ops[0].reg)
   assert isinstance(dest,int),('unknown branch',hex(pc))
   if dest!=target:
    if mn in ('bl','blr'):
     v=dict(values);v['x30']=pc+4
     next_pc(dest,v,returns+((pc+4,values.get('sp')),))
    else:next_pc(dest)
  elif mn=='ret':
   r=ops[0].reg if ops else None
   dest=value(r) if r is not None else values.get('x30')
   if not returns:
    assert pc==page_return and dest==('caller',) and values.get('sp')==('stack',0),('RET before stock or unknown return',hex(pc))
   else:
    assert dest==returns[-1][0] and values.get('sp')==returns[-1][1],('RET before stock or unknown return',hex(pc))
    next_pc(dest,values,returns[:-1])
  elif mn.startswith('b.') or mn in ('cbz','cbnz','tbz','tbnz'):
   # Never search for one successful path: BOTH must reach stock. Abstract
   # conditions may over-reject, but cannot hide an early-return alternative.
   assert ops[-1].type==ARM64_OP_IMM,('unknown conditional branch',hex(pc))
   next_pc(ops[-1].imm);next_pc(pc+4)
  else:
   assert mn in plain,('unsupported forward instruction',hex(pc),mn)
   if mn in ('mrs','msr'):
    assert i.op_str.endswith('nzcv') if mn=='mrs' else i.op_str.startswith('nzcv,'),('unsupported system register',hex(pc))
   if mn in ('str','stp'):
    mem=ops[-1] if ops[-1].type==ARM64_OP_MEM else ops[-2]
    assert mem.type==ARM64_OP_MEM and key(i,mem.mem.base)=='sp' and not mem.mem.index,('unproved store',hex(pc))
   writes=i.regs_access()[1]
   if any(key(i,r)=='sp' for r in writes):
    assert (mn in ('add','sub') and key(i,ops[1].reg)=='sp' and ops[2].type==ARM64_OP_IMM) or (mn in ('stp','str','ldp','ldr') and i.writeback),('unproved SP write',hex(pc))
   v=dict(values);st=dict(stack)
   for r in writes:v.pop(key(i,r),None)
   if mn=='mov' and ops[0].type==ARM64_OP_REG:
    val=ops[1].imm if ops[1].type==ARM64_OP_IMM else value(ops[1].reg) if ops[1].type==ARM64_OP_REG else None
    if val is not None:v[key(i,ops[0].reg)]=val & ((1<<(width(ops[0].reg)*8))-1) if isinstance(val,int) else val if width(ops[0].reg)==8 else None
   if mn=='movk' and isinstance(value(ops[0].reg),int):
    shift=ops[1].shift.value;mask=(1<<(32 if i.reg_name(ops[0].reg).startswith('w') else 64))-1
    v[key(i,ops[0].reg)]=((value(ops[0].reg)&~(65535<<shift))|(ops[1].imm<<shift))&mask
   # Byte-addressed stack cells: partial/overlapping stores invalidate old
   # saved values. A W load cannot restore a 64-bit return address token.
   if mn in ('add','sub') and ops[1].type==ARM64_OP_REG and ops[2].type==ARM64_OP_IMM:
    a=value(ops[1].reg);b=ops[2].imm << ops[2].shift.value
    if mn=='sub':b=-b
    if isinstance(a,tuple) and a[0]=='stack' and width(ops[0].reg)==8:v[key(i,ops[0].reg)]=('stack',a[1]+b)
   if mn in ('str','stp','ldr','ldp','ldur'):
    mem=next(o for o in ops if o.type==ARM64_OP_MEM);base=value(mem.mem.base)
    if isinstance(base,tuple) and base[0]=='stack' and not mem.mem.index:
     at=base[1]+mem.mem.disp
     regs=[o.reg for o in ops if o.type==ARM64_OP_REG]
     for r in regs:
      n=width(r)
      if mn in ('str','stp'):
       for k in list(st):
        if max(k[0],at)<min(k[0]+k[1],at+n):del st[k]
       val=value(r)
       if val is not None:st[(at,n)]=val
      else:
       val=st.get((at,n))
       if val is not None:v[key(i,r)]=val
      at+=n
     if i.writeback:
      delta=ops[-1].imm if ops[-1].type==ARM64_OP_IMM else mem.mem.disp
      v[key(i,mem.mem.base)]=('stack',base[1]+delta)
    elif mn in ('str','stp'):raise AssertionError(('unknown stack store',hex(pc)))
   v={k:val for k,val in v.items() if val is not None};v.pop('xzr',None)
   next_pc(pc+4,v,st=st)
  active.remove(state);cache[state]=True
 walk(entry,{'sp':('stack',0),'x30':('caller',)},(),{},0)
 return count

# Independent SHA-pinned instruction paths for the three C absolute FN targets.
# finalize's suffix is evidence for the stock address, NOT a candidate entry.
BASE_FN_PATHS={'live_open_wrap':0x401e3f7c,
               'live_finalize_wrap':0x401e4620,'live_event_wrap':0x401e463c}

def patch_module(src):
 spec=importlib.util.spec_from_file_location('recorder_patch',src/'recorder_patch.py')
 m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def branch(data,at):
 w=struct.unpack_from('<I',data,at-BASE)[0]
 if w>>26 not in (5,37):return None
 return at+(((w&0x3ffffff)^0x2000000)-0x2000000)*4,w>>26==37

class ForwardProofFailure(AssertionError):
 def __init__(self, report):
  self.report=report
  failures={k:v for k,v in report['forwarding'].items() if v['status']=='FAIL'}
  super().__init__(('old forwarding proof failed',failures))

def verify(base,final,elf_path,hook_obj,src):
 m=patch_module(src);b=base.read_bytes();f=final.read_bytes()
 assert hashlib.sha256(b).hexdigest()==m.SHA,'wrong base SHA'
 assert len(b)==len(f),'image length changed'
 e=ELFFile(io.BytesIO(elf_path.read_bytes()))
 assert e['e_machine']=='EM_AARCH64' and e['e_type']=='ET_EXEC'
 sy=e.get_section_by_name('.symtab');assert sy is not None
 symbols={s.name:s['st_value'] for s in sy.iter_symbols() if s['st_shndx']!='SHN_UNDEF'}
 assert not [s.name for s in sy.iter_symbols() if s.name and s['st_shndx']=='SHN_UNDEF'],'unresolved symbols'
 high=START;covered=bytearray(LIMIT-START)
 for sec in e.iter_sections():
  if not sec['sh_flags']&2 or not sec['sh_size']:continue
  at,n=sec['sh_addr'],sec['sh_size']
  assert START<=at and at+n<=LIMIT,(sec.name,'payload limit')
  assert not sec['sh_flags']&1 and sec['sh_type']!='SHT_NOBITS',(sec.name,'writable state')
  assert f[at-BASE:at-BASE+n]==sec.data(),(sec.name,'ELF/MAIN mismatch')
  assert not any(covered[at-START:at-START+n]),'overlapping sections'
  covered[at-START:at-START+n]=b'\1'*n;high=max(high,at+n)
 assert high>START
 for off,v in enumerate(covered):
  assert v or f[START-BASE+off] in (0,b[START-BASE+off]),'unmapped payload byte'
 assert high<=PINNED,'linked Live overlaps pinned core helpers'
 assert f[PINNED-BASE:LIMIT-BASE]==b[PINNED-BASE:LIMIT-BASE],'pinned core helpers changed'
 for name,(pc,target) in BASE_TRANSFERS.items():
  assert branch(b,pc)[0]==target,(name,'baseline evidence mismatch')
 sites=dict(OLD);defs=dict(OLD_DEFS);m.extend_relive(defs,sites)
 for ev,at,raw,kind,target in m.PROBES:
  assert b[at-BASE:at-BASE+4].hex()==raw
 for name,v in defs.items():assert symbols[name]==v,(name,'wrong continuation')
 for at,(name,call) in sites.items():
  assert START<=symbols[name]<high
  assert branch(f,at)==(symbols[name],call),(hex(at),name,'wrong patch target')
 # SHA-fixed evidence for absolute targets, independent of candidate symbols.
 for name,entry in BASE_FN_PATHS.items():
  prove_forward(lambda pc,n:b[pc-BASE:pc-BASE+n],entry,FORWARDS[name])
 forwarding={}
 # Start at the fixed external patch site. Its B/BL destination is read from
 # MAIN; candidate symbol values/sizes NEVER establish proof entry or bounds.
 for site,(name,call) in OLD.items():
  if name not in FORWARDS:continue
  entry=branch(f,site)[0]
  def read_path(pc,n):
   assert START<=pc and pc+n<=PINNED,(name,'forward path escaped',hex(pc))
   assert all(covered[pc-START:pc-START+n]),(name,'unmapped forward instruction',hex(pc))
   return f[pc-BASE:pc-BASE+n]
  try:
   states=prove_forward(read_path,entry,FORWARDS[name],page_contract=True)
   forwarding[name]=dict(status='PASS',entry=hex(entry),states=states)
  except AssertionError as error:
   forwarding[name]=dict(status='FAIL',entry=hex(entry),reason=str(error))
 # Two additional core branches are NOT Live hooks: retain targets + helper bytes.
 for at,target,call in [(0x401cf98c,0x401fe000,False),(0x401d8984,0x401fe070,True)]:
  assert branch(b,at)==(target,call) and branch(f,at)==(target,call)
 allowed=bytearray(len(b));allowed[START-BASE:PINNED-BASE]=b'\1'*(PINNED-START)
 for at in sites:allowed[at-BASE:at-BASE+4]=b'\1'*4
 assert all(x==y or allowed[i] for i,(x,y) in enumerate(zip(b,f))),'patch whitelist violation'
 # Relocate the COMPLETE hooks.S .text from its final build object and compare.
 # This covers fast gates, restores, displaced instructions, literals, B/BL and CBZ/TBZ expansion.
 o=ELFFile(io.BytesIO(hook_obj.read_bytes()));os=o.get_section_by_name('.symtab')
 ot=o.get_section_by_name('.text');code=bytearray(ot.data())
 anchor=next(s for s in os.iter_symbols() if s.name=='live_irq_entry')
 start=symbols['live_irq_entry']-anchor['st_value']
 rel=o.get_section_by_name('.rela.text');assert rel is not None
 for r in rel.iter_relocations():
  symbol=os.get_symbol(r['r_info_sym']);name=symbol.name
  if symbol['st_shndx']=='SHN_UNDEF':target=symbols[name]
  elif symbol['st_shndx']==anchor['st_shndx']:target=start+symbol['st_value']
  else:raise AssertionError(('unsupported hook section',name))
  target+=r['r_addend'];off=r['r_offset'];kind=r['r_info_type']
  if kind==257:struct.pack_into('<Q',code,off,target)
  elif kind in (282,283):
   delta=target-(start+off);assert delta%4==0 and -(1<<27)<=delta<(1<<27)
   word=struct.unpack_from('<I',code,off)[0]
   assert word>>26==(37 if kind==283 else 5)
   struct.pack_into('<I',code,off,(word&0xfc000000)|((delta//4)&0x3ffffff))
  else:raise AssertionError(('unsupported relocation',kind))
 assert f[start-BASE:start-BASE+len(code)]==code,'linked trampoline relocation mismatch'
 report=dict(base_sha256=hashlib.sha256(b).hexdigest(),
             elf_sha256=hashlib.sha256(elf_path.read_bytes()).hexdigest(),
             final_sha256=hashlib.sha256(f).hexdigest(),payload_end=hex(high),
             patch_sites=len(sites),hardware_tested=False,forwarding=forwarding,
             structural_checks='PASS: ELF/MAIN, pinned targets, whitelist, complete hook relocation',
             status='PASS: final ELF/MAIN static checks only')
 if any(v['status']=='FAIL' for v in forwarding.values()):
  report['status']='FAIL: forwarding proof incomplete'
  raise ForwardProofFailure(report)
 return report
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('base','final','elf','hooks','src'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args()
 try:report=verify(a.base,a.final,a.elf,a.hooks,a.src)
 except ForwardProofFailure as error:
  print(json.dumps(error.report,indent=2));raise SystemExit(1)
 except AssertionError as error:
  print(json.dumps(dict(status='FAIL',reason=str(error),hardware_tested=False),indent=2));raise SystemExit(1)
 else:print(json.dumps(report,indent=2))
