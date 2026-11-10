#!/usr/bin/env python3
"""bg34 + compiled-object static audit; no device operations."""
import hashlib,importlib.util,io,json,struct,sys
from pathlib import Path
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
from elftools.elf.elffile import ELFFile
H=Path(__file__).resolve().parent
sp=importlib.util.spec_from_file_location('patch',H.parent/'recorder_patch.py');m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
def main(build):
 b=(m.ROOT/'bg34.main').read_bytes();assert hashlib.sha256(b).hexdigest()==m.SHA
 d=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN);lines=['bg34.main SHA256 '+m.SHA,'仅静态反汇编证据；无硬件验收。']
 ranges=[(a-4,12) for _,a,_,_,_ in m.PROBES]+[(0x402f2d58,24),(0x40c15c3c,0x44),(0x402dd4f8,0xa8),(0x402dd78c,0xed),(0x402dd8e8,0x6c),(0x408b6968,0xb4),(0x408b82ac,0xc8),(0x40c33148,0x110),(0x40c3ed44,0x64),(0x40c466e4,0xe8)]
 for at,n in ranges:
  lines.append('\n'+hex(at))
  for i in d.disasm(b[at-m.BASE:at-m.BASE+n],at):lines.append(f'{i.address:08x} {i.bytes.hex()} {i.mnemonic} {i.op_str}')
 for ev,a,raw,kind,t in m.PROBES:
  assert b[a-m.BASE:a-m.BASE+4].hex()==raw
  ins=list(d.disasm(bytes.fromhex(raw),a))[0]
  assert ins.mnemonic=={'cbz':'cbz','tbz':'tbz','call':'bl','plain':{4:'stp',5:'mov',14:'sub'}.get(ev)}[kind]
  if t:assert ins.op_str.endswith(hex(t))
 # Prove original hand-back body was not text-edited, including its DELAY/timeouts.
 orig=(m.SRC/'codec/live_w5enc.c').read_text();changed=(build/'codec/live_w5enc.c').read_text()
 assert changed==orig+m.SNAP
 f=ELFFile(io.BytesIO((build/'recorder-check/recorder_recorder.c.o').read_bytes()))
 asm=list(d.disasm(f.get_section_by_name('.text').data(),0))
 assert any(i.mnemonic=='ldaxr' for i in asm) and any(i.mnemonic=='stlxr' for i in asm)
 for i in asm:
  if i.mnemonic=='stlxr':
   operands=i.op_str.split(', ');assert operands[0]!=operands[1]
 undef=[s.name for s in f.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']=='SHN_UNDEF' and s.name]
 assert set(undef)<= {'fr_encoder_snapshot','fr_context'},undef
 (build/'recorder-evidence.txt').write_text('\n'.join(lines)+'\n')
 print('PASS: base instruction bytes/decoded targets, unchanged hand-back source, object inline atomics. Final linked relocations NOT checked here.')
 print('Evidence:',build/'recorder-evidence.txt')
if __name__=='__main__':main(Path(sys.argv[1]))
