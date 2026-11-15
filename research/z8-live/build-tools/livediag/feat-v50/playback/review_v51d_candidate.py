#!/usr/bin/env python3
"""Independent read-only audit of v51 candidate, whitelist and manifest replay."""
from pathlib import Path
import hashlib,json,struct,subprocess
H=Path.home();D=H/'fxfix/v51d';V=H/'v37build/v51d';B=0x40160000;ID=0x401fe060-B
sha=lambda b:hashlib.sha256(b).hexdigest()
base=(H/'fxfix/v50/q/main.main').read_bytes();old=(H/'v37build/v50b/v37.main').read_bytes();new=(V/'v37.main').read_bytes();final=(D/'q/main.main').read_bytes()
assert sha(base)=='4cb5235fc709220f64af5400680653999d2a03b0d5a4821975e0f6a7b2d50977'
assert sha(old)=='de78b3a28609a1539461ac0bdf9be9f644c277935ad1d605425f967e74e19c2a'
assert sha(new)=='736aa971ba99a0aef3e5d155fd9db3f7c028adb09f9576761407502d2ac36e70'
assert sha(final)=='aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
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
meta=bytes.fromhex('2a8c8e52aaacad721f000a6bc0008052e003801ac0035fd6')
allow(0x401fd200,len(meta),'apple_meta');allow(0x40cea514,4,'apple_meta_hook')
assert not any(expected[0x401fd200-B:0x401fd200-B+len(meta)])
assert expected[0x40cea514-B:0x40cea518-B]==bytes.fromhex('e0071f32')
expected[0x401fd200-B:0x401fd200-B+len(meta)]=meta
struct.pack_into('<I',expected,0x40cea514-B,0x94000000|(((0x401fd200-0x40cea514)//4)&0x3ffffff))
# Previously integrated portrait/size patch bytes must remain unchanged.
for p in json.loads((H/'fxfix/v50/portrait.json').read_text())['patches']:
 a=int(p['address'],16)-B;b=a+len(bytes.fromhex(p['after']));assert final[a:b]==base[a:b]==bytes.fromhex(p['after'])
for a,n in [(0x401fd000,196),(0x401fd100,60),(0x402f3264,4),(0x402f3c54,4)]:assert final[a-B:a-B+n]==base[a-B:a-B+n]
struct.pack_into('<I',expected,ID,0);bid=struct.unpack('<I',hashlib.sha256(expected).digest()[:4])[0]|0x01000000;struct.pack_into('<I',expected,ID,bid)
assert expected==final
assert all(allowed[i] for i,(a,b) in enumerate(zip(base,final)) if a!=b)
# Replay the entire final harness manifest against its pinned reference.
m=json.loads((D/'q/changed-ranges.json').read_text());ref=(H/'fxfix/base-c770f340.main').read_bytes();replay=bytearray(ref);end=0
for r in m['ranges']:
 at=int(r['address'],16)-B;a=bytes.fromhex(r['expected']);b=bytes.fromhex(r['replacement']);assert len(a)==len(b)==r['size'];assert at>=end and ref[at:at+len(a)]==a
 replay[at:at+len(a)]=b;end=at+len(a)
assert replay==final and m['output_sha256']==sha(final)
syms=json.loads((H/'fxfix/v50/q/changed-ranges.json').read_text())['symbols']
for part in ['live','pb']:syms.update(json.loads((V/part/'changed-ranges.json').read_text())['symbols'])
assert m['symbols']==syms
# Byte-exact source preservation, not merely version comments.
source_changes=[];source_checked=0
for part in ['src-fix37','pb-fix37']:
 A=H/'v37build/v50b/work'/part;C=V/'work'/part
 for p in A.rglob('*'):
  if p.is_file() and not p.name.startswith('._') and p.suffix in ['.c','.h','.S']:
   q=C/p.relative_to(A);source_checked+=1
   if not q.exists() or p.read_bytes()!=q.read_bytes():source_changes.append(f'{part}/{p.relative_to(A)}')
assert 'src-fix37/runtime.c' not in source_changes, source_changes
assert (V/'work/src-fix37/runtime.c').read_bytes()==(H/'v37build/v50b/work/src-fix37/runtime.c').read_bytes()
ui=(V/'work/src-fix37/ui/live_ui.c').read_text();stream=(V/'work/src-fix37/capture/live_stream.c').read_text()
assert '(void)live_font_fix;' in ui and 'Live Photo: Preparing' in ui
assert 'else if(status&LIVE_REQUESTED){LABEL("Live Photo: Requested");}' not in ui
runtime=(V/'work/src-fix37/runtime.c').read_text();assert '*got*LIVE_AUDIO_CHANNELS' in runtime
print(json.dumps({'result':'passed','main_sha256':sha(final),'v50_sha256':sha(base),'v51d_sha256':sha(new),'changed_bytes_vs_v50':sum(a!=b for a,b in zip(base,final)),'rebuilt_layer_changed_bytes':len(rebuilt_delta),'allowed_spans':len(blocks),'payloads':[x for x in blocks if x['name'] in ['live','pb','apple_meta']],'source_files_compared':source_checked,'source_changes':source_changes,'font_retarget_disabled':True,'live_status_preparing':True,'v50_handoff_gain_runtime_byte_exact':True,'portrait_and_size_patches_unchanged':True,'symbols_match_v51d':True,'manifest_replays_full_image':True,'build_id':hex(bid),'scope':'read-only source and complete MAIN audit; no camera or new simulation'},indent=2))
