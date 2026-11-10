#!/usr/bin/env python3
"""Interpret compiled full-frame shells and displaced instructions; no emulator.
C stubs destroy caller-saved state/SIMD. Only pinned X19 is ABI-preserved across
I/notify; original X19 still must be restored from the outer frame.
"""
import io,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN

def main(build):
 e=ELFFile(io.BytesIO((build/'recorder-check/hooks.S.o').read_bytes()))
 tab=e.get_section_by_name('.symtab');sy={s.name:s['st_value'] for s in tab.iter_symbols()}
 d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN);d.detail=True
 ins={i.address:i for i in d.disasm(e.get_section_by_name('.text').data(),0)}
 rel={r['r_offset']:tab.get_symbol(r['r_info_sym']).name for r in e.get_section_by_name('.rela.text').iter_relocations()}
 destinations={'fr_next_2':0x402f3040,'fr_target_2':0x402f305c,'fr_next_3':0x402f306c}
 cases=0
 for event in (1,2,3):
  entry=sy['live_w5_bringup_entry' if event==1 else f'fr_entry_{event}']
  start=next(pc for pc,i in ins.items() if entry<=pc and i.mnemonic=='sub')
  for accepted in (0,1):
   for raw in (0,1,5,0x80000000,0xffffffff,0x123400000000):
    for flags in range(16):
     regs={f'x{i}':0x1234567800000000+i for i in range(31)}
     regs.update({f'q{i}':(1<<100)+i for i in range(32)})
     regs.update(x0=raw,sp=0x100000,nzcv=flags<<28,fpcr=0x1234,fpsr=0x4321)
     orig=dict(regs);mem={orig['sp']+0x70:0xabcdef,orig['sp']+0x78:0x40d07ba8}
     pc=start;seen=[];low=regs['sp'];session=0xaaa000 if accepted else 0
     def key(n):return {'fp':'x29','lr':'x30'}.get(n,'x'+n[1:] if n.startswith('w') else n)
     def read(n):
      v=0 if n in ('xzr','wzr') else regs[key(n)]
      return v&0xffffffff if n.startswith('w') else v
     def write(n,v):
      if n not in ('xzr','wzr'):regs[key(n)]=v&((1<<32)-1 if n.startswith('w') else (1<<128)-1)
     for _ in range(300):
      if event==1 and pc==sy['fr_stock_1']:break
      if event!=1 and pc in destinations.values():break
      i=ins[pc];op=i.operands;mn=i.mnemonic;rn=lambda o:i.reg_name(o.reg)
      value=lambda o:read(rn(o)) if o.type==1 else o.imm
      nxt=pc+4
      if mn in ('sub','add'):write(rn(op[0]),value(op[1])+(-1 if mn=='sub' else 1)*value(op[2]))
      elif mn=='mov':write(rn(op[0]),value(op[1]))
      elif mn in ('mrs','msr'):
       dst,src=i.op_str.split(', ');write(dst,read(src))
      elif mn in ('stp','ldp','str','ldr'):
       pair=mn in ('stp','ldp');o=op[2 if pair else 1];addr=read(i.reg_name(o.mem.base))+o.mem.disp
       assert not i.writeback
       for k in range(2 if pair else 1):
        reg=rn(op[k]);at=addr+k*(16 if reg.startswith('q') else 8)
        if mn.startswith('st'):mem[at]=read(reg)
        else:write(reg,mem[at])
      elif mn=='bl':
       name=rel[pc];seen.append(name)
       assert regs['sp']==orig['sp']-784
       if name=='fr_before':assert regs['x0']==orig['sp']-256
       if name=='live_w5_bringup_notify_lamp':
        assert all(regs[f'x{k}']==orig[f'x{k}'] for k in range(8))
        assert regs['nzcv']==orig['nzcv'] and regs['x19']==session
       if name=='fr_light':
        stage=0 if len(seen)==2 else 1
        assert regs['x0']==session and regs['x1']==stage and regs['x2']==orig['sp']-256
        assert mem[regs['x2']]==orig['x0']
       if name=='fr_probe':
        assert regs['x0']==event and regs['x1']==orig['sp']-256
        assert mem[regs['x1']]==orig['x0'] and mem[regs['x1']+152]==orig['x19']
       for r in regs:
        if r not in ('sp','x19'):regs[r]=0xbad
       regs['x0']=session if name=='fr_before' else 0xdead
      elif mn in ('cbz','cbnz'):
       if bool(value(op[0]))==(mn=='cbnz'):nxt=op[1].imm
      elif mn=='b':nxt=destinations[rel[pc]] if pc in rel else op[0].imm
      else:raise AssertionError((mn,i.op_str))
      low=min(low,regs['sp']);pc=nxt
     else:raise AssertionError('nonterminating shell')
     expected=dict(orig)
     if event==3:expected.update(x19=0xabcdef,x30=0x40d07ba8)
     assert regs==expected,(event,regs,expected)
     assert orig['sp']-low==784
     if event==1:assert seen==['fr_before','fr_light','live_w5_bringup_notify_lamp','fr_light']
     else:
      assert seen==['fr_probe']
      assert pc==(0x402f306c if event==3 else 0x402f3040 if raw&0xffffffff else 0x402f305c)
     cases+=1
 # Bring-up replay is SUB SP,SP,#0x80 followed by the pinned continuation.
 assert ins[sy['fr_stock_1']].mnemonic=='sub' and ins[sy['fr_stock_1']].op_str=='sp, sp, #0x80'
 print(f'PASS: {cases} compiled-shell paths; full 784-byte x0-x30/SIMD/NZCV/FPCR/FPSR restore; exact I/R order, W0 CBZ and M LDP replay')
 print('LIMIT: instruction subset and C stubs; no native scheduler/filesystem or hardware execution')
if __name__=='__main__':main(Path(sys.argv[1]))
