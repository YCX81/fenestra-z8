"""Strictly split linked primary Live and .live_extra without copying the hole.

Optional --image verifies the primary was already applied, then adds only the
zero-guarded extra cave and two original-word-guarded diagnostic BL sites.
No updater/card/device operation is supported.
"""
import argparse,hashlib,json,struct
from pathlib import Path
LOAD=0x40160000;START=0x401e2000;LIMIT=0x401fd000;EXTRA=0x401fd218;END=0x401fe000
def sha(b):return hashlib.sha256(b).hexdigest()
def split(path):
 b=Path(path).read_bytes();assert b[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',b,18)[0]==183
 shoff=struct.unpack_from('<Q',b,40)[0];ents,num,names=struct.unpack_from('<HHH',b,58)
 assert ents==64 and shoff+num*ents<=len(b)
 h=[struct.unpack_from('<IIQQQQIIQQ',b,shoff+i*64) for i in range(num)]
 n=h[names];strs=b[n[4]:n[4]+n[5]]
 names=[strs[x[0]:].split(b'\0',1)[0].decode() for x in h]
 regions=[[],[]];symbols={}
 for name,s in zip(names,h):
  typ,flags,addr,off,size=s[1:6]
  if typ==2:
   st=h[s[6]];strings=b[st[4]:st[4]+st[5]];assert s[9]==24
   for at in range(off,off+size,24):
    nm,info,other,idx,value,sz=struct.unpack_from('<IBBHQQ',b,at)
    symbols[strings[nm:].split(b'\0',1)[0].decode()]=value
  if not flags&2 or not size:continue
  assert not flags&1 and typ!=8,(name,'writable/zero-fill alloc forbidden')
  assert off+size<=len(b)
  which=0 if START<=addr and addr+size<=LIMIT else 1 if EXTRA<=addr and addr+size<=END else -1
  assert which>=0,(name,hex(addr),size,'outside allowed caves')
  if which==1:assert name=='.live_extra',(name,'extra cave must be explicit section')
  regions[which].append((addr,name,b[off:off+size]))
 out=[];sections=[]
 for i,rs in enumerate(regions):
  start=(START,EXTRA)[i];assert rs and min(x[0] for x in rs)==start
  rs.sort();data=bytearray();end=start
  for addr,name,payload in rs:
   assert addr>=end,(name,'overlap');data.extend(bytes(addr-end));data.extend(payload);end=addr+len(payload)
   sections.append({'name':name,'address':hex(addr),'size':len(payload),'sha256':sha(payload)})
  out.append(bytes(data))
 return out,symbols,sections
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',type=Path,required=True);p.add_argument('--out-dir',type=Path,required=True)
 p.add_argument('--image',type=Path);p.add_argument('--expected-sha256');p.add_argument('--output',type=Path)
 a=p.parse_args();(primary,extra),symbols,sections=split(a.elf);a.out_dir.mkdir(parents=True,exist_ok=True)
 (a.out_dir/'primary.bin').write_bytes(primary);(a.out_dir/'extra.bin').write_bytes(extra)
 report={'elf':str(a.elf),'elf_sha256':sha(a.elf.read_bytes()),'primary_address':hex(START),'primary_bytes':len(primary),'primary_sha256':sha(primary),'extra_address':hex(EXTRA),'extra_bytes':len(extra),'extra_sha256':sha(extra),'sections':sections,'gap_copied':False}
 if a.image:
  assert a.expected_sha256 and a.output
  source=a.image.read_bytes();assert sha(source)==a.expected_sha256
  assert source[START-LOAD:START-LOAD+len(primary)]==primary,'primary image does not match this ELF'
  changes=[(EXTRA,bytes(len(extra)),extra,'Live extra cave')]
  for site,old,name in [(0x40f6cff0,'37bf0094','live_shutter_prepare_entry'),(0x40f6e174,'f8ba0094','live_shutter_fire_entry')]:
   delta=symbols[name]-site;assert delta%4==0 and -(1<<27)<=delta<1<<27
   changes.append((site,bytes.fromhex(old),struct.pack('<I',0x94000000|((delta//4)&0x3ffffff)),name))
  result=bytearray(source);ranges=[]
  for addr,expected,new,why in changes:
   at=addr-LOAD;assert len(new)==len(expected) and source[at:at+len(new)]==expected,(hex(addr),'expected bytes mismatch')
   assert not any(addr<x['end'] and x['start']<addr+len(new) for x in ranges)
   result[at:at+len(new)]=new;ranges.append({'start':addr,'end':addr+len(new),'expected':expected.hex(),'replacement':new.hex(),'purpose':why})
  restored=bytearray(result)
  for r in ranges:restored[r['start']-LOAD:r['end']-LOAD]=bytes.fromhex(r['expected'])
  assert restored==source and result[LIMIT-LOAD:EXTRA-LOAD]==source[LIMIT-LOAD:EXTRA-LOAD]
  a.output.write_bytes(result);report.update(input_sha256=sha(source),output_sha256=sha(result),ranges=ranges,reconstruction_equal=True,size_patch_gap_unchanged=True)
 (a.out_dir/'sections.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in ('ranges','sections')},indent=2))
if __name__=='__main__':main()
