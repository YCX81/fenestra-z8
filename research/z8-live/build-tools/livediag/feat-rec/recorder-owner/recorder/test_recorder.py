#!/usr/bin/env python3
"""Offline decoder and fast-gate structure regression. No emulator/hardware claim."""
import importlib.util,struct,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
import decoder

def decode_test():
 data=bytearray(2048)
 for i,ev in enumerate((0,1,2,13)):
  p=[0]*128;p[:5]=[0x32524657,2,i,ev,0];p[8]=42;p[118]=7
  p[119:123]=[16]*i+[0]*(4-i);p[123]=i;p[124]=11
  b=bytearray(struct.pack('<128I',*p));struct.pack_into('<I',b,508,decoder.checksum(b[:508]));data[i*512:(i+1)*512]=b
 rows,err=decoder.decode(data);assert len(rows)==4 and not err
 assert rows[3]['事件号']==13 and rows[3]['REC代次']==7
 data[512+99]^=1;rows,err=decoder.decode(data);assert len(rows)==3 and len(err)==1
 assert decoder.decode(data[:800])[1]
 print('PASS: v2 decoder, transaction fields, checksum corruption, truncation')
def gate_test(build):
 with (build/'recorder-check/hooks.S.o').open('rb') as f:
  e=ELFFile(f);sym=e.get_section_by_name('.symtab');code=e.get_section_by_name('.text').data()
  calls={r['r_offset']:sym.get_symbol(r['r_info_sym']).name for r in e.get_section_by_name('.rela.text').iter_relocations() if r['r_info_type']==283}
  d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
  for s in sym.iter_symbols():
   if not s.name.startswith('fr_entry_'):continue
   ins=list(d.disasm(code[s['st_value']:],s['st_value']))
   first=next(i for i in ins if i.mnemonic=='bl');assert calls[first.address]=='fr_enabled'
   assert not any('q0' in i.op_str for i in ins if i.address<first.address)
   assert ins[ins.index(first)+1].mnemonic=='cbz'
 print('PASS: every new probe gates before SIMD save; static structure only')
if __name__=='__main__':decode_test();gate_test(Path(sys.argv[1]))
