"""C3.11: accept Apple timed-metadata handler 'meta' in unused track kind 4.
Other unknown handlers retain stock error 6. This is an experimental writer lane.
The containing function saves/restores LR; x10 is caller-saved and dead at return.
"""
from pathlib import Path
import hashlib,json,struct,sys
BASE=0x40160000
SITE=0x40cea514
CAVE=0x401fd200
ORIGINAL=bytes.fromhex('e0071f32')

def code():
 import subprocess,tempfile,io
 from elftools.elf.elffile import ELFFile
 asm='.text\nmov w10, #0x7461\nmovk w10, #0x6d65, lsl #16\ncmp w0, w10\nmov w0, #6\ncsel w0, wzr, w0, eq\nret\n'
 with tempfile.TemporaryDirectory() as tmp:
  obj=Path(tmp)/'meta.o'
  subprocess.run(['clang','--target=aarch64-none-elf','-x','assembler','-c','-o',str(obj),'-'],input=asm.encode(),check=True)
  elf=ELFFile(io.BytesIO(obj.read_bytes()))
  assert elf.get_section_by_name('.rela.text') is None
  return elf.get_section_by_name('.text').data()

def apply(data):
 d=bytearray(data);blob=code()
 assert d[SITE-BASE:SITE-BASE+4]==ORIGINAL,'parser hook is not stock'
 assert not any(d[CAVE-BASE:CAVE-BASE+len(blob)]),'metadata cave occupied'
 d[CAVE-BASE:CAVE-BASE+len(blob)]=blob
 struct.pack_into('<I',d,SITE-BASE,0x94000000|(((CAVE-SITE)//4)&0x3ffffff))
 return bytes(d)

if __name__=='__main__':
 a,b=map(Path,sys.argv[1:]);d=a.read_bytes();out=apply(d);b.write_bytes(out)
 print(json.dumps(dict(input_sha256=hashlib.sha256(d).hexdigest(),output_sha256=hashlib.sha256(out).hexdigest(),site=hex(SITE),cave=hex(CAVE),code=code().hex(),experimental=True)))
