#!/usr/bin/env python3
"""Synthetic ELF fixture for verifier negative tests. NOT a firmware/full-link test."""
import io,struct,sys,tempfile
from pathlib import Path
from elftools.elf.elffile import ELFFile
from verify_link import BASE,START,OLD,OLD_DEFS,FORWARDS,prove_forward,patch_module,verify

def main(build):
 with tempfile.TemporaryDirectory(prefix='verify-link-',dir=build/'recorder-check') as out:
  run(build,Path(out))

def run(build,out):
 root=Path(__file__).resolve().parents[2];m=patch_module(root/'src-fix35')
 obj=build/'recorder-check/hooks.S.o';raw=obj.read_bytes();e=ELFFile(io.BytesIO(raw))
 tab=e.get_section_by_name('.symtab');text=e.get_section_by_name('.text');code=bytearray(text.data())
 symbols={s.name:START+s['st_value'] for s in tab.iter_symbols() if s.name and s['st_shndx']==e.get_section_index('.text')}
 defs=dict(OLD_DEFS);sites=dict(OLD);m.extend_relive(defs,sites)
 symbols.update(defs)
 for name in sorted({s.name for s in tab.iter_symbols() if s.name and s['st_shndx']=='SHN_UNDEF'} | {n for n,_ in OLD.values()}):
  if name not in symbols:
   symbols[name]=START+len(code)
   if name in FORWARDS:code+=struct.pack('<I',0x14000000|(((FORWARDS[name]-symbols[name])//4)&0x3ffffff))
   else:code+=bytes.fromhex('c0035fd6')
 for r in e.get_section_by_name('.rela.text').iter_relocations():
  s=tab.get_symbol(r['r_info_sym']);v=symbols[s.name]+r['r_addend'];at=r['r_offset'];kind=r['r_info_type']
  if kind==257:struct.pack_into('<Q',code,at,v)
  elif kind in (282,283):
   w=struct.unpack_from('<I',code,at)[0];struct.pack_into('<I',code,at,(w&0xfc000000)|(((v-START-at)//4)&0x3ffffff))
  else:raise AssertionError(kind)
 # Isolated RET/dead-forward probes: no candidate symbol defines proof bounds.
 probes={}
 for name,target in FORWARDS.items():
  at=START+len(code);probes[name]=at
  code+=bytes.fromhex('c0035fd6')+bytes(24)
  code+=struct.pack('<I',0x14000000|(((target-(at+28))//4)&0x3ffffff))
 # Minimal ET_EXEC with linked hook bytes and stub destinations. No runtime/native execution.
 strings=b'\0';symdata=bytes(24)
 for name,v in symbols.items():
  index=len(strings);strings+=name.encode()+b'\0'
  symdata+=struct.pack('<IBBHQQ',index,0x12,0,1 if START<=v<START+len(code) else 0xfff1,v,0)
 shstrings=b'\0.text\0.symtab\0.strtab\0.shstrtab\0'
 data=bytearray(64);sections=[bytes(64)]
 for name,typ,flags,addr,content,link,info,align,entsize in [
  (1,1,6,START,code,0,0,16,0),(7,2,0,0,symdata,3,1,8,24),
  (15,3,0,0,strings,0,0,1,0),(23,3,0,0,shstrings,0,0,1,0)]:
  data+=bytes((-len(data))%align);off=len(data);data+=content
  sections.append(struct.pack('<IIQQQQIIQQ',name,typ,flags,addr,off,len(content),link,info,align,entsize))
 data+=bytes((-len(data))%8);shoff=len(data);data+=b''.join(sections)
 ident=b'\x7fELF\x02\x01\x01'+bytes(9)
 data[:64]=struct.pack('<16sHHIQQQIHHHHHH',ident,2,183,1,START,0,shoff,0,64,0,0,64,5,4)
 elf=out/'synthetic.elf';elf.write_bytes(data)
 base=root/'bg34.main';image=bytearray(base.read_bytes());image[START-BASE:START-BASE+len(code)]=code
 for at,(name,call) in sites.items():struct.pack_into('<I',image,at-BASE,(0x94000000 if call else 0x14000000)|(((symbols[name]-at)//4)&0x3ffffff))
 final=out/'synthetic.main';final.write_bytes(image)
 check=lambda:verify(base,final,elf,obj,root/'src-fix35')
 check();cases=0
 for at in (0x40300000,0x402f303c,0x40ea3638,0x401fff80,0x401fe000):
  broken=bytearray(image);broken[at-BASE]^=4;final.write_bytes(broken)
  try:check()
  except AssertionError:cases+=1
  else:raise AssertionError(('accepted corrupt image',hex(at)))
 final.write_bytes(image)
 broken=bytearray(data);struct.pack_into('<Q',broken,shoff+64+16,0x401fff80);elf.write_bytes(broken)
 try:check()
 except AssertionError:cases+=1
 else:raise AssertionError('accepted out-of-range ELF')
 elf.write_bytes(data)
 # Verify trampoline reconstruction independently from ELF/MAIN correspondence.
 broken=bytearray(raw);broken[text['sh_offset']]^=4;badobj=out/'bad-hooks.o';badobj.write_bytes(broken)
 try:verify(base,final,elf,badobj,root/'src-fix35')
 except AssertionError:cases+=1
 else:raise AssertionError('accepted mismatched hook object')
 print('PASS: synthetic valid fixture and',cases,'negative cases; NOT a real full payload link')
 # Corrupt BOTH symbol and corresponding branch/literal, keeping ELF/MAIN
 # consistent. This reproduced the old verifier hole; every pinned symbol fails.
 se=ELFFile(io.BytesIO(data));st=se.get_section_by_name('.symtab');ct=se.get_section_by_name('.text')
 for name in OLD_DEFS:
  bad=START+len(code)-4;broken=bytearray(data);badimage=bytearray(image)
  index=next(i for i,s in enumerate(st.iter_symbols()) if s.name==name)
  struct.pack_into('<Q',broken,st['sh_offset']+index*24+8,bad)
  for r in e.get_section_by_name('.rela.text').iter_relocations():
   if tab.get_symbol(r['r_info_sym']).name!=name:continue
   off=r['r_offset'];kind=r['r_info_type'];v=bad+r['r_addend']
   if kind==257:patch=struct.pack('<Q',v)
   else:
    w=struct.unpack_from('<I',code,off)[0]
    patch=struct.pack('<I',(w&0xfc000000)|(((v-START-off)//4)&0x3ffffff))
   broken[ct['sh_offset']+off:ct['sh_offset']+off+len(patch)]=patch
   badimage[START-BASE+off:START-BASE+off+len(patch)]=patch
  # Forwarding fixture branches outside hooks.S must also agree with bad symbol.
  for fn,target in FORWARDS.items():
   if target!=OLD_DEFS[name] or symbols[fn]<START+len(text.data()):continue
   off=symbols[fn]-START;patch=struct.pack('<I',0x14000000|(((bad-symbols[fn])//4)&0x3ffffff))
   broken[ct['sh_offset']+off:ct['sh_offset']+off+4]=patch
   badimage[START-BASE+off:START-BASE+off+4]=patch
  elf.write_bytes(broken);final.write_bytes(badimage)
  try:check()
  except AssertionError as error:
   assert name in str(error),str(error);cases+=1
  else:raise AssertionError(('accepted self-consistent wrong symbol/branch',name))
 elf.write_bytes(data);final.write_bytes(image)
 # Also reject C inline forwarding corruption with no symbol to pin.
 for name in ('live_open_wrap','live_finalize_wrap','live_event_wrap'):
  off=symbols[name]-START;broken=bytearray(data);badimage=bytearray(image)
  patch=bytes.fromhex('c0035fd6')
  broken[ct['sh_offset']+off:ct['sh_offset']+off+4]=patch
  badimage[START-BASE+off:START-BASE+off+4]=patch
  elf.write_bytes(broken);final.write_bytes(badimage)
  try:check()
  except AssertionError as error:
   assert name in str(error);cases+=1
  else:raise AssertionError(('accepted wrong inline forwarding',name))
 elf.write_bytes(data)
 final.write_bytes(image)
 # REVIEW3: symbol entry -> RET, matching external patch branch, st_size
 # includes the real stock branch after that RET. Each wrapper uses its own
 # fixture probe; addresses move when the bring-up shell grows.
 def reject(name,broken,badimage,label):
  nonlocal cases
  elf.write_bytes(broken);final.write_bytes(badimage)
  try:check()
  except AssertionError as error:
   assert name in str(error) and 'old forwarding proof failed' in str(error),(label,str(error))
   cases+=1;print('REJECT:',label,name,str(error))
  else:raise AssertionError(('accepted self-consistent ELF/MAIN',label,name))
 def set_symbol(broken,name,at,size):
  index=next(i for i,s in enumerate(st.iter_symbols()) if s.name==name)
  struct.pack_into('<QQ',broken,st['sh_offset']+index*24+8,at,size)
 def set_code(broken,badimage,at,blob):
  off=ct['sh_offset']+at-START
  broken[off:off+len(blob)]=blob;badimage[at-BASE:at-BASE+len(blob)]=blob
 def jump(at,dest,call=False):
  return struct.pack('<I',(0x94000000 if call else 0x14000000)|(((dest-at)//4)&0x3ffffff))
 for name in FORWARDS:
  broken=bytearray(data);badimage=bytearray(image)
  at=probes[name]
  assert image[at-BASE:at-BASE+4]==bytes.fromhex('c0035fd6')
  set_symbol(broken,name,at,0x20)
  site,call=next((a,c) for a,(n,c) in OLD.items() if n==name)
  badimage[site-BASE:site-BASE+4]=jump(site,at,call)
  reject(name,broken,badimage,'REVIEW3 RET entry + patch + covering size')
 # Additional adversarial self-consistent ELF/MAINs. Keep entry/symbol/patch
 # unchanged; redirect the reachable code to probes with tempting dead targets.
 name='live_open_wrap';entry=symbols[name];at=probes[name];target=FORWARDS[name]
 variants={
  'skip good branch to RET':jump(at,at+8)+jump(at+4,target)+bytes.fromhex('c0035fd6'),
  'one conditional arm returns':struct.pack('<I',0xb4000040)+jump(at+4,target)+bytes.fromhex('c0035fd6'),
  'cycle before dead target':jump(at,at)+jump(at+4,target),
  'unknown BR before dead target':bytes.fromhex('20001fd6')+jump(at+4,target),
  'escape fixed payload':jump(at,0x401fe000)+jump(at+4,target),
  'callee loops before dead target':jump(at,at+8,True)+jump(at+4,target)+jump(at+8,at+8),
  'callee corrupts LR':jump(at,at+12,True)+jump(at+4,target)+bytes(4)+bytes.fromhex('1e0080d2c0035fd6'),
  'W overwrite clears known target':bytes.fromhex('08649652a810a8720800805200011fd6'),
  'unknown RET register in callee':jump(at,at+12,True)+jump(at+4,target)+bytes(4)+bytes.fromhex('00005fd6'),
  'unsupported trap before dead target':bytes.fromhex('000020d4')+jump(at+4,target),
  # Lane 0 contains stock; lane 1 is unknown. V0 is not a scalar.
  'SIMD lane alias smuggling':bytes.fromhex('08649652a810a872001d084e083c184e00011fd6'),
  # MOV W8, low; MOVK W8, high; MOV X8, X0; BR X8; dead correct B.
  'constant target clobbered':struct.pack('<II',0x52800008|((target&65535)<<5),0x72a00008|((target>>16)<<5))+bytes.fromhex('e80300aa00011fd6')+jump(at+16,target),
 }
 for label,blob in variants.items():
  broken=bytearray(data);badimage=bytearray(image)
  set_symbol(broken,name,entry,at+32-entry)
  set_code(broken,badimage,entry,jump(entry,at))
  set_code(broken,badimage,at,blob)
  reject(name,broken,badimage,label)
 # Positive CFG controls: real branch destinations, both conditional arms,
 # and an indirect constant target. An intentionally tiny st_size must not
 # determine proof bounds, just as an oversized one must not hide dead code.
 positives=[jump(at,at+8)+bytes(4)+jump(at+8,target),
            struct.pack('<I',0xb4000040)+jump(at+4,target)+jump(at+8,target),
            bytes.fromhex('08649652a810a87200011fd6')]
 for blob in positives:
  broken=bytearray(data);goodimage=bytearray(image)
  set_symbol(broken,name,entry,1)
  set_code(broken,goodimage,entry,jump(entry,at));set_code(broken,goodimage,at,blob)
  elf.write_bytes(broken);final.write_bytes(goodimage);check()
 elf.write_bytes(data);final.write_bytes(image);check()
 # Preserve the original real-compiler absolute-target recognition checks,
 # now by execution of fixed reviewed instruction paths instead of a scan.
 # finalize is ONLY its terminal suffix, not a proof of its full object CFG
 # (the object has unresolved calls and retry loops).
 real=ELFFile(io.BytesIO((build/'recorder-check/runtime.c.o').read_bytes()))
 body=real.get_section_by_name('.text').data()
 rs={s.name:s for s in real.get_section_by_name('.symtab').iter_symbols()}
 for name,length,suffix in [('live_open_wrap',44,False),('live_finalize_wrap',28,True),('live_event_wrap',36,False)]:
  symbol=rs[name];entry=symbol['st_value']+(symbol['st_size']-length if suffix else 0);end=entry+length
  prove_forward(lambda pc,n:body[pc:pc+n],entry,FORWARDS[name],entry,end)
 print('PASS: synthetic fixture + 3 CFG positives and',cases,'negative cases; real compiler open/event prefixes and finalize suffix; NOT a real full payload link')
if __name__=='__main__':main(Path(sys.argv[1]))
