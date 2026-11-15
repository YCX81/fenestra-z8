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
 regions=[[],[],[]];symbols={}
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
  which=0 if START<=addr and addr+size<=LIMIT else 1 if EXTRA<=addr and addr+size<=END else 2 if 0x4019f020<=addr and addr+size<=0x4019ff40 else -1
  assert which>=0,(name,hex(addr),size,'outside allowed caves')
  if which==1:assert name=='.live_extra',(name,'extra cave must be explicit section')
  if which==2:assert name=='.live_shutter',(name,'shutter bank must be explicit section')
  regions[which].append((addr,name,b[off:off+size]))
 out=[];sections=[]
 for i,rs in enumerate(regions):
  start=(START,EXTRA,0x4019f020)[i];assert rs and min(x[0] for x in rs)==start
  rs.sort();data=bytearray();end=start
  for addr,name,payload in rs:
   assert addr>=end,(name,'overlap');data.extend(bytes(addr-end));data.extend(payload);end=addr+len(payload)
   sections.append({'name':name,'address':hex(addr),'size':len(payload),'sha256':sha(payload)})
  out.append(bytes(data))
 return out,symbols,sections
