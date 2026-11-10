#!/usr/bin/env python3
"""Stack/conditional-return regression on actual linked bytes; no CPU emulator."""
import io,struct,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN
from verify_link import prove_forward,BASE,START,PINNED,FORWARDS

def main(root):
 p=root/'linked-recorder';f=(p/'live-fixed.emulation-only.main').read_bytes()
 elf=ELFFile(io.BytesIO((p/'payload.elf').read_bytes()))
 sy={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
 entry=sy['live_ui_menu_page'];target=FORWARDS['live_ui_menu_page']
 read=lambda pc,n:f[pc-BASE:pc-BASE+n]
 assert prove_forward(read,entry,target,page_contract=True)>0
 count=0
 # Every actual RET instruction, as the entry of every old forwarding hook.
 # Symbol size cannot change this proof: no ELF symbol metadata is passed.
 text=elf.get_section_by_name('.text')
 rets=[i.address for i in Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN).disasm(text.data(),text['sh_addr']) if i.mnemonic=='ret']
 for t in FORWARDS.values():
  for pc in rets:
   try:prove_forward(read,pc,t,page_contract=True)
   except AssertionError:count+=1
   else:raise AssertionError(('accepted actual RET entry',hex(pc),hex(t)))
 # Every guarded-page instruction mutation must lose its return permission.
 # Replace guard, output expression, restore or transfer with NOP or RET.
 for off in (0,4,8,12,16,20,24,32,36,40,44,52):
  for word in (0xd503201f,0xd65f03c0):
   b=bytearray(f);struct.pack_into('<I',b,entry-BASE+off,word)
   try:prove_forward(lambda pc,n:b[pc-BASE:pc-BASE+n],entry,target,page_contract=True)
   except AssertionError:count+=1
   else:raise AssertionError(('accepted page mutation',off,hex(word)))
 # A RET substituted at either lookup helper entry is NOT source semantics.
 for name in ('ui','core_root'):
  b=bytearray(f);struct.pack_into('<I',b,sy[name]-BASE,0xd65f03c0)
  try:prove_forward(lambda pc,n:b[pc-BASE:pc-BASE+n],entry,target,page_contract=True)
  except AssertionError:count+=1
  else:raise AssertionError(('accepted corrupted source helper',name))
 # No page contract: same real function must still reject the outer return.
 try:prove_forward(read,entry,target)
 except AssertionError as e:assert hex(entry+48) in str(e)
 else:raise AssertionError('uncontracted wrapper returned')
 # Scalar stack alias/width regressions. BL helper; B target; helper saves LR,
 # calls another leaf, restores and returns. All targets are actual bytes.
 a=START;t=0x4085b320
 def b(pc,dest,call=False):return struct.pack('<I',(0x94000000 if call else 0x14000000)|(((dest-pc)//4)&0x3ffffff))
 good=b(a,a+8,True)+b(a+4,t)+bytes.fromhex('fd7bbfa9')+b(a+12,a+24,True)+bytes.fromhex('fd7bc1a8c0035fd6c0035fd6')
 assert prove_forward(lambda pc,n:good[pc-a:pc-a+n],a,t)>0
 for label,patch in [
  ('overwrite saved LR',bytes.fromhex('ff0700f9')),
  ('partial overwrite saved LR',bytes.fromhex('ff0b00b9')),
  ('unbalanced SP',bytes.fromhex('ff4300d1')),
  ('unknown LR',bytes.fromhex('fe0300aa'))]:
  # Insert the damaging instruction in the leaf, before its RET.
  bad=good[:-4]+patch+good[-4:]
  try:prove_forward(lambda pc,n:bad[pc-a:pc-a+n],a,t)
  except AssertionError:count+=1
  else:raise AssertionError(('accepted stack damage',label))
 print(f'PASS: real guarded page, nested stack return; {count} negatives including {len(rets)} RET entries x {len(FORWARDS)} wrappers')
if __name__=='__main__':main(Path(__file__).resolve().parents[2])
