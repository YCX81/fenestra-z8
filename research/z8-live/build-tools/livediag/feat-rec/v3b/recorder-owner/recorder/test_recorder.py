#!/usr/bin/env python3
"""Offline decoder and fast-gate structure regression. No emulator/hardware claim."""
import importlib.util,struct,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
import decoder
import arm

def decode_test():
 data=bytearray(3072)
 for i,ev in enumerate(decoder.EVENTS):
  p=[0]*128;p[:5]=[0x33524657,3,i,ev,0];p[8]=42;p[118]=7
  p[119:125]=[16]*i+[0]*(6-i);p[125]=1;p[126]=9;p[5]=1
  b=bytearray(struct.pack('<128I',*p));struct.pack_into('<I',b,508,decoder.checksum(b[:508]));data[i*512:(i+1)*512]=b
 rows,err=decoder.decode(data);assert len(rows)==6 and not err
 assert rows[5]['事件号']==13 and rows[5]['REC代次']==7
 assert rows[1]['当前runtime_epoch']==9 and rows[1]['shutdown']==1 and rows[1]['原因']==['epoch变化(仅标记)']
 old=bytearray(data);struct.pack_into('<II',old,0,0x32524657,2)
 assert decoder.decode(old)[1]
 wrong=bytearray(data);struct.pack_into('<I',wrong,12,1);struct.pack_into('<I',wrong,508,decoder.checksum(wrong[:508]));assert decoder.decode(wrong)[1]
 data[512+99]^=1;rows,err=decoder.decode(data);assert len(rows)==5 and len(err)==1
 assert decoder.decode(data[:800])[1]
 assert rows[0]['轻量记录'] and not rows[0]['Live有效']
 print('PASS: v3 fixed-sector decoder, old format and wrong event rejected, transaction fields, checksum corruption, truncation')
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
def status_test(build):
 l=arm.layout(build/'recorder-check/layout.o')
 assert l['runtime_size']==26512 and l['tx_offset']==104 and l['file_bytes']==3072 and l['transaction_slots']==6
 reads=[]
 def read(at,n):
  reads.append((at,n));return struct.pack('<I',{0x1000+1744:19,0x1000+1740:1}[at])
 assert arm.read_runtime_status(read,0x1000,l)==(19,1)
 assert reads==[(0x1000+1744,4),(0x1000+1740,4)]
 print('PASS: AArch64 layout size and offline runtime epoch/shutdown reads')
if __name__=='__main__':decode_test();gate_test(Path(sys.argv[1]));status_test(Path(sys.argv[1]))
