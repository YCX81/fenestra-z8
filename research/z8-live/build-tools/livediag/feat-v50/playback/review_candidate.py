#!/usr/bin/env python3
"""Independent read-only audit of v50 candidate, whitelist and manifest replay."""
from pathlib import Path
import hashlib,json,struct,subprocess
H=Path.home();D=H/'fxfix/v50';V=H/'v37build/v50b';B=0x40160000;ID=0x401fe060-B
sha=lambda b:hashlib.sha256(b).hexdigest()
base=(H/'fxfix/v49/q/main.main').read_bytes();old=(H/'v37build/v49/v37.main').read_bytes();new=(V/'v37.main').read_bytes();final=(D/'q/main.main').read_bytes()
assert sha(base)=='63240425765acad54030e58f2dad83b2f4679a9e511a7e5b03e2c4ba1a28718a'
assert sha(old)=='4bf1a2d60d58ce6aaf9cd2738f83a0b3fc483dc266c3fc4021778e48edfeb286'
assert sha(new)=='de78b3a28609a1539461ac0bdf9be9f644c277935ad1d605425f967e74e19c2a'
assert sha(final)=='4cb5235fc709220f64af5400680653999d2a03b0d5a4821975e0f6a7b2d50977'
assert len(base)==len(old)==len(new)==len(final)
# Explicit allowed code/hooks, independent of compose's unconstrained runs().
hooks=[0x403db694,0x4032e130,0x40d60ec8,0x40fdc9c8,0x40e90838,0x40c26804,0x40c26818,0x40c26828,0x40e8b328,0x408b7c1c,0x406cd6a0,0x406cd75c,0x40ea3638,0x406cd468,0x41020844,0x40d60ecc,0x4085c424,0x4085bcb0,0x401cce84,0x401cd3ec,0x4061a724,0x40619de4,0x40618be0,0x40618b84,0x40f9cb64,0x402f2d58]
blocks=[];allowed=bytearray(len(final))
def allow(address,length,label):
 a=address-B;b=a+length;assert a>=0 and b<=len(final) and not any(allowed[a:b]),label
 allowed[a:b]=b'\1'*length;blocks.append({'name':label,'address':hex(address),'bytes':length})
for part,start in [('live',0x401e2000),('pb',0x40196000)]:
 code=(V/part/'payload.bin').read_bytes();oldcode=(H/'v37build/v49'/part/'payload.bin').read_bytes()
 allow(start,max(len(code),len(oldcode)),part)
 assert final[start-B:start-B+len(code)]==code
 assert new[start-B:start-B+len(code)]==code
for h in hooks:allow(h,4,'layer_hook')
allow(B+ID,4,'build_id')
rebuilt_delta=[i for i,(a,b) in enumerate(zip(old,new)) if a!=b]
assert all(allowed[i] for i in rebuilt_delta)
expected=bytearray(base)
for i in rebuilt_delta:
 if ID<=i<ID+4:continue
 assert base[i]==old[i],hex(i+B)
 expected[i]=new[i]
portrait=json.loads((D/'portrait.json').read_text())['patches']
for p in portrait:
 at=int(p['address'],16);before=bytes.fromhex(p['before']);after=bytes.fromhex(p['after']);assert len(before)==len(after)
 allow(at,len(after),p['kind']);assert expected[at-B:at-B+len(before)]==before
 expected[at-B:at-B+len(after)]=after
for stem,at,hook,orig,call in [('liveplan-stock',0x401fd000,0x402f3264,'092c4029',False),('livefbh',0x401fd100,0x402f3c54,'086d1c12',True)]:
 code=(D/'playback'/f'{stem}.bin').read_bytes();assert code==(H/'fxfix/liveplay'/f'{stem}.bin').read_bytes()
 allow(at,len(code),stem);allow(hook,4,stem+'_hook')
 assert not any(expected[at-B:at-B+len(code)+32]);assert expected[hook-B:hook-B+4]==bytes.fromhex(orig)
 expected[at-B:at-B+len(code)]=code;struct.pack_into('<I',expected,hook-B,(0x94000000 if call else 0x14000000)|(((at-hook)//4)&0x3ffffff))
struct.pack_into('<I',expected,ID,0);bid=struct.unpack('<I',hashlib.sha256(expected).digest()[:4])[0]|0x01000000;struct.pack_into('<I',expected,ID,bid)
assert expected==final
assert all(allowed[i] for i,(a,b) in enumerate(zip(base,final)) if a!=b)
# Replay the entire final harness manifest against its pinned reference.
m=json.loads((D/'q/changed-ranges.json').read_text());ref=(H/'fxfix/base-c770f340.main').read_bytes();replay=bytearray(ref);end=0
for r in m['ranges']:
 at=int(r['address'],16)-B;a=bytes.fromhex(r['expected']);b=bytes.fromhex(r['replacement']);assert len(a)==len(b)==r['size'];assert at>=end and ref[at:at+len(a)]==a
 replay[at:at+len(a)]=b;end=at+len(a)
assert replay==final and m['output_sha256']==sha(final)
syms=json.loads((H/'fxfix/v49/q/changed-ranges.json').read_text())['symbols']
for part in ['live','pb']:syms.update(json.loads((V/part/'changed-ranges.json').read_text())['symbols'])
assert m['symbols']==syms
# Byte-exact source preservation, not merely version comments.
source_changes=[];source_checked=0
for part in ['src-fix37','pb-fix37']:
 A=H/'v37build/v49/work'/part;C=V/'work'/part
 for p in A.rglob('*'):
  if p.is_file() and not p.name.startswith('._') and p.suffix in ['.c','.h','.S']:
   q=C/p.relative_to(A);source_checked+=1
   if not q.exists() or p.read_bytes()!=q.read_bytes():source_changes.append(f'{part}/{p.relative_to(A)}')
assert source_changes==['src-fix37/runtime.c'],source_changes
ui=(V/'work/src-fix37/ui/live_ui.c').read_text();stream=(V/'work/src-fix37/capture/live_stream.c').read_text()
assert '(void)live_font_fix;' in ui and 'Live Photo: Preparing' in ui
assert 'else if(status&LIVE_REQUESTED){LABEL("Live Photo: Requested");}' not in ui
runtime=(V/'work/src-fix37/runtime.c').read_text();assert '*got*LIVE_AUDIO_CHANNELS' in runtime
print(json.dumps({'result':'passed','main_sha256':sha(final),'v49_sha256':sha(base),'v50b_sha256':sha(new),'changed_bytes_vs_v49':sum(a!=b for a,b in zip(base,final)),'rebuilt_layer_changed_bytes':len(rebuilt_delta),'allowed_spans':len(blocks),'payloads':[x for x in blocks if x['name'] in ['live','pb','liveplan-stock','livefbh']],'source_files_compared':source_checked,'source_changes':source_changes,'font_retarget_disabled':True,'live_status_preparing':True,'v47_v48_source_byte_exact_to_v49':True,'symbols_match_v50b':True,'manifest_replays_full_image':True,'build_id':hex(bid),'scope':'read-only source and complete MAIN audit; no camera or new simulation'},indent=2))
