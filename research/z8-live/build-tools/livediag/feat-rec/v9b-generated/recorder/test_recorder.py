#!/usr/bin/env python3
"""v4 file-format, full-frame call placement and unchanged arm layout checks."""
import struct,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
import decoder,arm

def decode_test():
 data=bytearray(3072)
 for i,ev in enumerate(decoder.EVENTS):
  p=[0]*128;p[:5]=[decoder.MAGIC,decoder.VERSION,i,ev,0];p[8]=42;p[118]=7
  p[119:125]=[16]*i+[0]*(6-i);p[125]=1;p[126]=9;p[5]=1
  b=bytearray(struct.pack('<128I',*p));struct.pack_into('<I',b,508,decoder.checksum(b[:508]));data[i*512:(i+1)*512]=b
 rows,err=decoder.decode(data);assert len(rows)==2 and not err
 assert rows[1]['事件号']==15 and rows[1]['REC代次']==7 and rows[1]['hand_back已返回'] is None
 assert rows[1]['当前runtime_epoch']==9 and rows[1]['shutdown']==1
 old=bytearray(data);struct.pack_into('<II',old,0,0x33524657,3);assert decoder.decode(old)[1]
 wrong=bytearray(data);struct.pack_into('<I',wrong,12,1);struct.pack_into('<I',wrong,508,decoder.checksum(wrong[:508]));assert decoder.decode(wrong)[1]
 wrong=bytearray(data);wrong[1024]=1;assert decoder.decode(wrong)[1]
 data[512+99]^=1;rows,err=decoder.decode(data);assert len(rows)==1 and len(err)==1
 assert decoder.decode(data[:800])[1]
 print('PASS: v4 fixed sectors, v3/wrong event/nonzero reserved/corruption/truncation rejection')
def gate_test(build):
 with (build/'recorder-check/hooks.S.o').open('rb') as f:
  e=ELFFile(f);sym=e.get_section_by_name('.symtab');code=e.get_section_by_name('.text').data()
  calls={r['r_offset']:sym.get_symbol(r['r_info_sym']).name for r in e.get_section_by_name('.rela.text').iter_relocations() if r['r_info_type']==283}
  d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
  entries=[s for s in sym.iter_symbols() if s.name.startswith('fr_entry_')]
  assert {s.name for s in entries}=={'fr_entry_2','fr_entry_3'}
  for s in entries:
   ins=list(d.disasm(code[s['st_value']:],s['st_value']))
   first=next(i for i in ins if i.mnemonic=='bl');assert calls[first.address]=='fr_probe'
   prefix=[i for i in ins if i.address<first.address]
   assert any(i.op_str=='q30, q31, [sp, #0x1e0]' for i in prefix)
   assert any(i.op_str=='x10, fpsr' for i in prefix)
 print('PASS: S/M first C call follows full SIMD/FPCR/FPSR save')
def status_test(build):
 l=arm.layout(build/'recorder-check/layout.o')
 assert l['runtime_size']==26512 and l['tx_offset']==104 and l['file_bytes']==3072 and l['transaction_slots']==6
 reads=[]
 def read(at,n):
  reads.append((at,n));return struct.pack('<I',{0x1000+1744:19,0x1000+1740:1}[at])
 assert arm.read_runtime_status(read,0x1000,l)==(19,1)
 assert reads==[(0x1000+1744,4),(0x1000+1740,4)]
 print('PASS: same AArch64 layout and offline arm status reads')
if __name__=='__main__':decode_test();gate_test(Path(sys.argv[1]));status_test(Path(sys.argv[1]))
