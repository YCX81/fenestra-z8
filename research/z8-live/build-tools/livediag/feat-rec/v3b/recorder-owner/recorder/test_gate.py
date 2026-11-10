#!/usr/bin/env python3
"""Interpret actual decoded gate instructions. No QEMU/Unicorn/device execution.
Only the explicitly implemented A64 subset is accepted; unknown instructions fail.
This checks control/data flow, not ARM hardware or microarchitectural semantics.
"""
import io,json,struct,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
CODE=0x100000;SP=0x208000;MASK=(1<<64)-1
ROOTSLOT=0xa032b50d8
ROOT,CORE,REG,UI,RT=(0xa01000000+i*0x100000 for i in range(5))

def execute(ins,entry,stop,mem,flags):
 regs={f'x{i}':0x1234567800000000+i*0x1020304 for i in range(31)}
 regs['sp']=SP;original=dict(regs);nzcv=flags;pc=entry;n=0;low=SP;writes=[]
 def readreg(r):return 0 if r in ('wzr','xzr') else regs[r.replace('w','x',1)]&((1<<32)-1 if r.startswith('w') else MASK)
 def writereg(r,v):
  if r not in ('wzr','xzr'):regs[r.replace('w','x',1)]=v&((1<<32)-1 if r.startswith('w') else MASK)
 def load(at,size):
  assert all(at+i in mem for i in range(size)),('unsafe/uninitialized read',hex(at),size)
  return int.from_bytes(bytes(mem[at+i] for i in range(size)),'little')
 def store(at,v):
  assert SP-32<=at and at+8<=SP,('unexpected stack/non-stack write',hex(at))
  for i,b in enumerate(v.to_bytes(8,'little')):mem[at+i]=b
  writes.append((at,8))
 for _ in range(300):
  if pc==stop:break
  i=ins[pc];op=i.operands;mn=i.mnemonic;r=lambda o:i.reg_name(o.reg)
  if mn=='sub': # First full frame instruction; only reachable on nonzero arm.
   assert i.op_str=='sp, sp, #0x100'
   assert regs==original and nzcv==flags
   return n,SP-low,'armed'
  n+=1;nextpc=pc+4
  val=lambda o:readreg(r(o)) if o.type==1 else o.imm<<o.shift.value
  if mn=='stp':
   at=readreg(i.reg_name(op[2].mem.base))+op[2].mem.disp
   store(at,readreg(r(op[0])));store(at+8,readreg(r(op[1])))
   if i.writeback:writereg('sp',at)
  elif mn=='ldp':
   at=readreg(i.reg_name(op[2].mem.base))+op[2].mem.disp
   writereg(r(op[0]),load(at,8));writereg(r(op[1]),load(at+8,8))
   if i.writeback:writereg('sp',readreg('sp')+op[3].imm)
  elif mn=='mrs':assert i.op_str=='x11, nzcv';writereg('x11',nzcv<<28)
  elif mn=='msr':assert i.op_str=='nzcv, x11';nzcv=readreg('x11')>>28
  elif mn=='mov':writereg(r(op[0]),val(op[1]))
  elif mn=='movk':
   shift=op[1].shift.value
   writereg(r(op[0]),(readreg(r(op[0]))&~(65535<<shift))|(op[1].imm<<shift))
  elif mn=='lsr':writereg(r(op[0]),val(op[1])>>op[2].imm)
  elif mn=='add':writereg(r(op[0]),val(op[1])+val(op[2]))
  elif mn in ('ldr','ldar','ldrh'):
   at=readreg(i.reg_name(op[1].mem.base))+op[1].mem.disp
   writereg(r(op[0]),load(at,2 if mn=='ldrh' else 4 if r(op[0]).startswith('w') else 8))
  elif mn=='cmp':
   bits=32 if r(op[0]).startswith('w') else 64;a=val(op[0]);b=val(op[1]);v=(a-b)&((1<<bits)-1)
   nzcv=((v>>(bits-1))<<3)|(int(v==0)<<2)|(int(a>=b)<<1)|int(bool((a^b)&(a^v)&(1<<(bits-1))))
  elif mn=='tst':
   v=val(op[0])&val(op[1]);nzcv=((v>>63)<<3)|(int(v==0)<<2)
  elif mn=='b.ne':
   if not nzcv&4:nextpc=op[0].imm
  elif mn=='b':nextpc=op[0].imm
  elif mn=='cbz':
   if not val(op[0]):nextpc=op[1].imm
  else:raise AssertionError(('unsupported instruction / C call on rejection',mn,i.op_str))
  low=min(low,readreg('sp'));pc=nextpc
 else:raise AssertionError('path did not terminate')
 assert regs==original,('register/SP corruption',regs,original)
 assert nzcv==flags,'NZCV corruption'
 assert writes==[(SP-32,8),(SP-24,8),(SP-16,8),(SP-8,8)]
 assert all(mem[SP+i]==0xa5 for i in range(256)),'live stack corruption'
 return n,SP-low,'rejected'

def main(build):
 e=ELFFile(io.BytesIO((build/'recorder-check/hooks.S.o').read_bytes()))
 tab=e.get_section_by_name('.symtab');code=e.get_section_by_name('.text').data()
 sy={s.name:CODE+s['st_value'] for s in tab.iter_symbols() if s['st_shndx']==e.get_section_index('.text')}
 d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN);d.detail=True
 ins={i.address:i for i in d.disasm(code,CODE)}
 slots=[ROOTSLOT,ROOT+63728,CORE+2528,REG+8,UI+8];good=[ROOT,CORE,REG,UI,RT]
 cases=[('zero_arm',None,None)]
 for i in range(5):
  for label,v in [('null',0),('unaligned',good[i]+1),('low',1),('high',0x1000000000),('end',0xa3ffffff8)]:cases.append((f'ptr{i}_{label}',slots[i],v))
 for addr in (CORE+64,CORE+68,CORE+32,REG,REG+4,UI,RT,RT+4):cases.append((f'identity_{addr:x}',addr,0))
 rows=[]
 for ev in (1,2,4,5,13,14):
  entry=sy['live_w5_bringup_entry' if ev==1 else f'fr_entry_{ev}']
  stop=sy['fr_bringup_unarmed' if ev==1 else f'fr_stock_{ev}']
  for name,addr,value in cases+[('armed',None,None)]:
   for flags in range(16):
    mem={SP+i:0xa5 for i in range(-32,256)}
    def put(at,v,n=8):mem.update({at+i:b for i,b in enumerate(int(v).to_bytes(n,'little'))})
    for at,v in zip(slots,good):put(at,v)
    for at,v in [(CORE+64,0x465a394d),(CORE+68,1),(CORE+32,46),(REG,0x47525846),(REG+4,64),(UI,0x4c505549),(RT,0x4c415231),(RT+4,26512)]:put(at,v,4)
    put(RT+0x5a90,int(name=='armed'),4)
    if addr is not None:put(addr,value,8 if addr in slots else 4)
    n,stack,result=execute(ins,entry,stop,mem,flags)
    assert result==('armed' if name=='armed' else 'rejected'),(ev,name,result)
    assert stack==32
    if name=='zero_arm':assert n==111
    if flags==0:rows.append(dict(event=ev,case=name,instructions=n,stack_bytes=stack))
 # Shared bring-up: exactly one full frame on each branch, no restore/re-save
 # between notify and probe. Check assembled frame locations and call ordering.
 start=sy['live_w5_bringup_entry'];end=sy['fr_stock_1'];split=sy['fr_bringup_unarmed']
 for lo,hi in [(start,split),(split,end)]:
  frame=[i for pc,i in ins.items() if lo<=pc<hi and i.mnemonic=='sub' and i.op_str=='sp, sp, #0x100']
  assert len(frame)==1
 calls=[(CODE+r['r_offset'],tab.get_symbol(r['r_info_sym']).name) for r in e.get_section_by_name('.rela.text').iter_relocations() if r['r_info_type']==283]
 assert [name for pc,name in calls if start<=pc<split]==['fr_before','live_w5_bringup_notify','fr_enabled','fr_probe']
 before=next(pc for pc,name in calls if start<=pc<split and name=='fr_before')
 notify=next(pc for pc,name in calls if start<=pc<split and name=='live_w5_bringup_notify')
 assert [(ins[pc].mnemonic,ins[pc].op_str) for pc in range(before+4,notify,4)]==[
  ('add','x9, sp, #0x210'),('ldp','x0, x1, [x9]'),('ldp','x2, x3, [x9, #0x10]'),
  ('ldp','x4, x5, [x9, #0x20]'),('ldp','x6, x7, [x9, #0x30]'),
  ('ldr','x9, [sp, #0x308]'),('msr','nzcv, x9')]
 assert [name for pc,name in calls if split<=pc<end]==['live_w5_bringup_notify']
 (build/'recorder-check/gate-results.json').write_text(json.dumps(rows,indent=2)+'\n')
 print('PASS:',len(rows)*16,'decoded-object paths; x0-x30/NZCV/SP/live stack preserved; only 32-byte scratch writes; no rejection C or SIMD instructions')
 print('Zero arm:',[(r['event'],r['instructions'],r['stack_bytes']) for r in rows if r['case']=='zero_arm'])
 print('Pointer/identity rejects:',min(r['instructions'] for r in rows if r['case'] not in ('armed','zero_arm')),'..',max(r['instructions'] for r in rows if r['case'] not in ('armed','zero_arm')),'instructions; 32 bytes')
 print('PASS: bring-up shares one full frame; LIMIT: subset interpreter + structural check, not ARM hardware/full payload execution')
if __name__=='__main__':main(Path(sys.argv[1]))
