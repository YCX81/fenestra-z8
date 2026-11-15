"""Fault tests for ELF cave extraction and guarded extra-cave composition."""
from pathlib import Path
import hashlib,json,struct,subprocess,tempfile
import split_live_payload as s
H=Path(__file__).resolve().parent
def elf(extra=s.EXTRA,writable=0):
 names=b'\0.text\0.live_extra\0.shstrtab\0.strtab\0.symtab\0'
 strings=b'\0live_shutter_prepare_entry\0live_shutter_fire_entry\0'
 syms=bytes(24)+struct.pack('<IBBHQQ',1,0x12,0,2,extra,4)+struct.pack('<IBBHQQ',28,0x12,0,2,extra+4,4)
 chunks=[b'',bytes.fromhex('c0035fd6'),bytes.fromhex('c0035fd6c0035fd6'),names,strings,syms]
 offsets=[];b=bytearray(64)
 for c in chunks:offsets.append(len(b));b+=c
 while len(b)%8:b+=b'\0'
 shoff=len(b)
 for i,c in enumerate(chunks):
  name=[0,1,7,19,29,37][i];typ=[0,1,1,3,3,2][i];flags=[0,6|writable,6,0,0,0][i]
  addr=s.START if i==1 else extra if i==2 else 0
  b+=struct.pack('<IIQQQQIIQQ',name,typ,flags,addr,offsets[i],len(c),4 if i==5 else 0,1 if i==5 else 0,8 if i==5 else 1,24 if i==5 else 0)
 ident=b'\x7fELF\x02\x01\x01'+bytes(9)
 b[:64]=struct.pack('<16sHHIQQQIHHHHHH',ident,2,183,1,s.START,0,shoff,0,64,0,0,64,6,3)
 return bytes(b)
with tempfile.TemporaryDirectory(dir=H,prefix='cave-test-') as td:
 t=Path(td);e=t/'test.elf';e.write_bytes(elf());parts,symbols,sections=s.split(e)
 assert parts==[bytes.fromhex('c0035fd6'),bytes.fromhex('c0035fd6c0035fd6')]
 assert symbols['live_shutter_fire_entry']==s.EXTRA+4
 image=bytearray(0x40f6e178-s.LOAD);image[s.START-s.LOAD:s.START-s.LOAD+4]=parts[0]
 image[s.LIMIT-s.LOAD:s.EXTRA-s.LOAD]=b'\xa5'*(s.EXTRA-s.LIMIT)
 image[0x40f6cff0-s.LOAD:0x40f6cff4-s.LOAD]=bytes.fromhex('37bf0094')
 image[0x40f6e174-s.LOAD:0x40f6e178-s.LOAD]=bytes.fromhex('f8ba0094')
 src=t/'source.main';src.write_bytes(image)
 cmd=['python3',str(H/'split_live_payload.py'),'--elf',str(e),'--out-dir',str(t/'out'),'--image',str(src),'--expected-sha256',hashlib.sha256(image).hexdigest(),'--output',str(t/'new.main')]
 subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)
 report=json.loads((t/'out/sections.json').read_text());assert report['reconstruction_equal'] and report['size_patch_gap_unchanged']
 for bad in (elf(s.LIMIT+4),elf(writable=1)):
  e.write_bytes(bad)
  try:s.split(e)
  except AssertionError:pass
  else:raise AssertionError('invalid cave accepted')
 e.write_bytes(elf());image[s.EXTRA-s.LOAD]=1;src.write_bytes(image)
 cmd[cmd.index('--expected-sha256')+1]=hashlib.sha256(image).hexdigest()
 p=subprocess.run(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);assert p.returncode
result={'passed':True,'cases':['split primary/extra','guards and full restoration','sizepatch gap unchanged','reject forbidden gap section','reject writable section','reject occupied extra cave']}
(H/'split-live-tests.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
