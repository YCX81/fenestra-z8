#!/usr/bin/env python3
"""Execute the compiled armed save/call/restore shell with hostile C stubs.
Small instruction interpreter, not QEMU, hardware, or execution of native I/O.
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
 calls={r['r_offset']:tab.get_symbol(r['r_info_sym']).name for r in e.get_section_by_name('.rela.text').iter_relocations() if r['r_info_type']==283}
 start=next(pc for pc,i in ins.items() if sy['live_w5_bringup_entry']<=pc<sy['fr_bringup_unarmed'] and i.mnemonic=='sub')
 for eligible in (0,1):
  for nzcv in range(16):
   regs={f'x{i}':0x1234567800000000+i for i in range(31)}
   regs.update({f'q{i}':(1<<100)+i for i in range(32)})
   regs.update(sp=0x100000,nzcv=nzcv<<28,fpcr=0x1234,fpsr=0x4321)
   orig=dict(regs);mem={};pc=start;seen=[];low=regs['sp']
   def read(name):return 0 if name in ('xzr','wzr') else regs[{'fp':'x29','lr':'x30'}.get(name,name.replace('w','x',1))]
   def write(name,val):
    if name not in ('xzr','wzr'):regs[{'fp':'x29','lr':'x30'}.get(name,name.replace('w','x',1))]=val&((1<<32)-1 if name.startswith('w') else (1<<128)-1)
   for _ in range(250):
    if pc==sy['fr_stock_1']:break
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
     name=calls[pc];seen.append(name)
     if name=='fr_before':assert regs['x0']==orig['sp']-256
     if name=='live_w5_bringup_notify':
      assert all(regs[f'x{k}']==orig[f'x{k}'] for k in range(8)), 'hand-back x0-x7 changed'
      assert regs['nzcv']==orig['nzcv'], 'hand-back NZCV changed'
      assert regs['sp']==orig['sp']-784, 'hand-back frame depth changed'
     if name in ('fr_enabled','fr_probe'):
      assert regs['x0']==1 and regs['x1']==orig['sp']-256
      assert mem[regs['x1']]==orig['x0'] and mem[regs['x1']+240]==orig['x30']
     # C stub returns (including simulated entry I/O failure); clobber all
     # registers and flags except SP to stress the outer save/restore shell.
     for r in regs:
      if r!='sp':regs[r]=0xbad
     regs['x0']=eligible if name=='fr_enabled' else 0xdead
    elif mn=='cbz':
     if value(op[0])==0:nxt=op[1].imm
    elif mn=='b':nxt=op[0].imm
    else:raise AssertionError((mn,i.op_str))
    low=min(low,regs['sp']);pc=nxt
   else:raise AssertionError('nonterminating shell')
   assert regs==orig,(regs,orig)
   assert orig['sp']-low==784
   assert seen==['fr_before','live_w5_bringup_notify','fr_enabled']+(['fr_probe'] if eligible else [])
 print('PASS: 32 armed shell paths; original x0-x7/NZCV at hand-back, SP=entry-784; x0-x30, q0-q31, NZCV, FPCR, FPSR, SP restored; 784-byte frame')
 print('LIMIT: C/native calls stubbed; a nonreturning I/O cannot be made safe by this shell')
if __name__=='__main__':main(Path(sys.argv[1]))
