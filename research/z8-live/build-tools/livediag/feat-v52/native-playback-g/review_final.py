from pathlib import Path
import hashlib,json,struct,subprocess,sys
V=sys.argv[1] if len(sys.argv)>1 else 'v52f'
H=Path('/path/to/user');B=H/'v37build'/V;Q=H/'fxfix/v52-root'/V/'q';OUT=H/'fxfix'/f'{V}-native'
LOAD=0x40160000;EXPECTED={'v52f':'ceefa749d298066d3666c083d4a15a1985939f22ca13ebd8d80244b28963857b','v52g':'ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613'}
sha=lambda b:hashlib.sha256(b).hexdigest()
main=(Q/'main.main').read_bytes();meta=json.loads((Q/'main.json').read_text());receipt=json.loads((B/'build.json').read_text());checks=[]
def check(name,truth,**detail):
 checks.append(dict(name=name,pass_=bool(truth),**detail))
 if not truth:raise AssertionError(name)
def section_data(path):
 b=path.read_bytes();assert b[:6]==b'\x7fELF\x02\x01';off=struct.unpack_from('<Q',b,40)[0];sz,n,si=struct.unpack_from('<HHH',b,58)
 ss=[struct.unpack_from('<IIQQQQIIQQ',b,off+i*sz) for i in range(n)]
 strs=b[ss[si][4]:ss[si][4]+ss[si][5]]
 out=[]
 for s in ss:
  name=strs[s[0]:].split(b'\0',1)[0].decode()
  if s[2]&2 and s[5]:out.append((name,s[1],s[3],b[s[4]:s[4]+s[5]],s[5]))
 return out
check('exact MAIN identity',sha(main)==EXPECTED.get(V,meta['sha256'])==meta['sha256'],sha256=sha(main))
regions=[]
for part in ['live','pb']:
 for name,typ,addr,data,size in section_data(B/part/'payload.elf'):
  check(f'{part} {name} is file backed',typ!=8,address=hex(addr),size=size)
  check(f'{part} {name} exact final bytes',main[addr-LOAD:addr-LOAD+size]==data)
  regions.append(dict(part=part,section=name,address=addr,end=addr+size,size=size,sha256=sha(data)))
for i,r in enumerate(regions):
 for s in regions[i+1:]:check(f'non-overlap {r["part"]}{r["section"]}/{s["part"]}{s["section"]}',r['end']<=s['address'] or s['end']<=r['address'])
for part,start,limit in [('live',0x401e2000,0x401fd000),('pb',0x40196000,0x401a0000)]:
 blob=(B/part/'payload.bin').read_bytes();check(part+' contiguous payload',main[start-LOAD:start-LOAD+len(blob)]==blob)
 check(part+' cave budget',start+len(blob)<=limit,bytes=len(blob),free=limit-start-len(blob))
extra=(B/'live/extra.bin').read_bytes();check('extra exact and fits',main[0x401fd218-LOAD:0x401fd218-LOAD+len(extra)]==extra and len(extra)<=0x401fe000-0x401fd218,bytes=len(extra),free=0x401fe000-0x401fd218-len(extra))
base=(H/'fxfix/v51d/q/main.main').read_bytes();old=(H/'v37build/v51d/v37.main').read_bytes();new=bytearray((B/'v37.main').read_bytes());at=0x401fe060-LOAD;new[at:at+4]=old[at:at+4]
check('fixed dimension/meta caves preserved',main[0x401fd000-LOAD:0x401fd218-LOAD]==base[0x401fd000-LOAD:0x401fd218-LOAD])
# Independently reapply only compiler delta and explicit reviewed overlay records.
rebuilt=bytearray(base);changed=0
for i in range(0,len(old),4096):
 if old[i:i+4096]==new[i:i+4096]:continue
 for j in range(i,min(i+4096,len(old))):
  if old[j]!=new[j]:
   assert base[j]==old[j],hex(j+LOAD)
   rebuilt[j]=new[j];changed+=1
for o in meta['extra_overlays']:
 addr=int(o['address'],16);p=addr-LOAD;n=o['size']
 if addr==0x401fd218:assert rebuilt[p:p+n]==bytes(n);replacement=extra
 else:
  assert addr in [0x40f6cff0,0x40f6e174] and n==4
  # Native original bytes are checked by original composition; no other hook accepted here.
  expect={0x40f6cff0:'37bf0094',0x40f6e174:'f8ba0094'}[addr]
  assert rebuilt[p:p+n].hex()==expect
  replacement=main[p:p+n]
 rebuilt[p:p+n]=replacement
for o in meta['evf_core_changes']:
 p=int(o['address'],16)-LOAD;before=bytes.fromhex(o['old']);after=bytes.fromhex(o['new']);assert rebuilt[p:p+len(before)]==before;rebuilt[p:p+len(after)]=after
portrait=json.loads((Q/'portrait-ranges.json').read_text())
for o in portrait:
 p=int(o['address'],16)-LOAD;before=bytes.fromhex(o['expected']);after=bytes.fromhex(o['replacement']);assert rebuilt[p:p+len(before)]==before;rebuilt[p:p+len(after)]=after
rebuilt[at:at+4]=bytes(4);id_=struct.unpack('<I',hashlib.sha256(rebuilt).digest()[:4])[0]|0x01000000;struct.pack_into('<I',rebuilt,at,id_)
check('independent full composition equals final',rebuilt==main,compiler_changed_bytes=changed,build_id=hex(id_),portrait_records=len(portrait))
manifest=json.loads((Q/'changed-ranges.json').read_text());aggregate=bytearray((H/'fxfix/base-c770f340.main').read_bytes())
for r in manifest['ranges']:
 p=int(r['address'],16)-LOAD;before=bytes.fromhex(r['expected']);after=bytes.fromhex(r['replacement']);assert len(before)==len(after)==r['size'];assert aggregate[p:p+len(before)]==before;aggregate[p:p+len(after)]=after
check('aggregate manifest complete and accurate',aggregate==main and manifest['output_sha256']==sha(main))
syms=json.loads((H/'fxfix/v51d/q/changed-ranges.json').read_text())['symbols']
for part in ['live','pb']:syms.update(json.loads((B/part/'changed-ranges.json').read_text())['symbols'])
check('published symbols equal final build manifests',syms==manifest['symbols'])
for part in ['live','pb']:
 undefined=subprocess.check_output(['llvm-nm-21','--undefined-only',str(B/part/'payload.elf')],text=True).strip();check(part+' no undefined symbols',not undefined)
source=B/'work/src-fix37/codec/live_w5enc.c'
result=dict(schema='v52.link-review/v1',version=V,main_sha256=sha(main),source_sha256=sha(source.read_bytes()),build_id=hex(id_),regions=regions,checks=checks,link_passed=all(c['pass_'] for c in checks),runtime_acceptance=False,scope='Byte identity/composition/cave/symbol audit only. Runtime and hardware acceptance are outside this link audit; no firmware or model changed.')
OUT.mkdir(exist_ok=True);(OUT/'linked-review.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ['checks','regions']},indent=2))
