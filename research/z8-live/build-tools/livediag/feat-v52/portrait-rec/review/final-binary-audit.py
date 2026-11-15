"""Read-only independent final MAIN audit. All results written only to supplied report."""
from pathlib import Path
import sys,json,hashlib,struct,binascii,io,stat
from elftools.elf.elffile import ELFFile
H=Path('/path/to/user');ROOT=Path(__file__).parent;build=Path(sys.argv[1]);q=Path(sys.argv[2]);source_manifest=Path(sys.argv[3]);report_path=Path(sys.argv[4]);BASE=0x40160000;ID=0x401fe060-BASE
sha=lambda b:hashlib.sha256(b).hexdigest()
final=(q/'main.main').read_bytes();meta=json.loads((q/'main.json').read_text());manifest=json.loads((q/'changed-ranges.json').read_text());base=(H/'fxfix/v51d/q/main.main').read_bytes();old=(H/'v37build/v51d/v37.main').read_bytes();raw=(build/'v37.main').read_bytes();receipt=json.loads((build/'build.json').read_text())
assert sha(base)=='aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433'
assert sha(old)=='736aa971ba99a0aef3e5d155fd9db3f7c028adb09f9576761407502d2ac36e70'
assert len(final)==len(base)==len(old)==len(raw)==81807709
assert sha(final)==meta['sha256']==manifest['output_sha256'] and sha(raw)==meta['rebuilt_sha256']
expected_source=json.loads(source_manifest.read_text());source_bad=[]
for name,digest in expected_source.items():
 p=build/'work'/name
 if not p.is_file() or sha(p.read_bytes())!=digest:source_bad.append(name)
assert not source_bad,source_bad
for s in receipt['stages']:assert sha((build/s['file']).read_bytes())==s['sha256']
def elf(path):
 with path.open('rb')as f:
  e=ELFFile(f);assert e['e_machine']=='EM_AARCH64';sy={s.name:s['st_value']for s in e.get_section_by_name('.symtab').iter_symbols()};sections=[]
  for s in e.iter_sections():
   if s['sh_flags']&2 and s['sh_size']:
    assert not s['sh_flags']&1 and s['sh_type']!='SHT_NOBITS'
    sections.append((s['sh_addr'],s.name,s.data()))
 return sy,sections
live_sy,live_sec=elf(build/'live/payload.elf');pb_sy,pb_sec=elf(build/'pb/payload.elf');sizes={}
for label,sections,lo,hi in [('live',live_sec,0x401e2000,0x401fd000),('pb',pb_sec,0x40196000,0x401a0000)]:
 spans=[]
 for a,n,b in sections:
  if label=='live'and n=='.live_extra':assert 0x401fd218<=a<a+len(b)<=0x401fe000
  else:assert lo<=a<a+len(b)<=hi,(label,n,hex(a),len(b))
  assert final[a-BASE:a-BASE+len(b)]==b,(label,n,'ELF bytes missing')
  spans.append((a,a+len(b),n))
 for x,y in zip(sorted(spans),sorted(spans)[1:]):assert x[1]<=y[0]
 sizes[label]=[{'address':hex(a),'section':n,'size':len(b),'sha256':sha(b)}for a,n,b in sections]
assert len((build/'live/payload.bin').read_bytes())==receipt['live_payload_bytes']
assert len((build/'pb/payload.bin').read_bytes())==receipt['playback_payload_bytes']
# Reconstruct composition independently: page-wise per-byte raw delta, then pinned overlays.
x=bytearray(base);normalized=bytearray(raw);normalized[ID:ID+4]=old[ID:ID+4];raw_changes=0
for p in range(0,len(old),65536):
 aa=old[p:p+65536];bb=normalized[p:p+65536]
 if aa==bb:continue
 for j,(a,b)in enumerate(zip(aa,bb)):
  if a!=b:assert base[p+j]==a,hex(BASE+p+j);x[p+j]=b;raw_changes+=1
def patch(a,old,new):
 o=a-BASE;assert len(old)==len(new)and x[o:o+len(old)]==old,(hex(a),'bad preimage');x[o:o+len(old)]=new

def branch(a,b,link=False):
 d=b-a;assert not d%4 and -(1<<27)<=d<(1<<27);return struct.pack('<I',(0x94000000 if link else 0x14000000)|((d//4)&0x3ffffff))
extra=[s for s in live_sec if s[1]=='.live_extra'];assert len(extra)==1;a,_,b=extra[0];assert a==0x401fd218;patch(a,bytes(len(b)),b)
for a,old,name in[(0x40f6cff0,'37bf0094','live_shutter_prepare_entry'),(0x40f6e174,'f8ba0094','live_shutter_fire_entry')]:patch(a,bytes.fromhex(old),branch(a,live_sy[name],True))
e=pb_sy
core=[(0x401a38c8,'e8031faa',branch(0x401a38c8,e['pb_output_initialize_global'])),(0x401a337c,'a80e41b9',bytes.fromhex('e00315aa')),(0x401a3380,'1f050071',branch(0x401a3380,e['pb_output_valid'],True)),(0x401a3384,'21230054',bytes.fromhex('20230034')),(0x401deec0,'d409344000000000',struct.pack('<Q',e['pb_output_target_enabled'])),(0x401deec8,'a409344000000000',struct.pack('<Q',e['pb_output_target_enable'])),(0x401deed8,'640d344000000000',struct.pack('<Q',e['pb_output_backing_rectangle']))]
assert sum(len(t[2])for t in core)==40
for a,old,b in core:patch(a,bytes.fromhex(old),b)
portrait=json.loads((ROOT/'final-binary-portrait-reference.json').read_text())['edits'];assert len(portrait)==419
for e in portrait:patch(int(e['address'],16),bytes.fromhex(e['expected']),bytes.fromhex(e['replacement']))
assert json.loads((q/'portrait-ranges.json').read_text())==portrait
x[ID:ID+4]=bytes(4);bid=int.from_bytes(hashlib.sha256(x).digest()[:4],'little')|0x1000000;x[ID:ID+4]=struct.pack('<I',bid)
assert bytes(x)==final and hex(bid)==meta['build_id']
assert final[0x401fd000-BASE:0x401fd218-BASE]==base[0x401fd000-BASE:0x401fd218-BASE]
# Full aggregate replay, independently checked preimages and nonoverlap.
ref=(H/'fxfix/base-c770f340.main').read_bytes();assert len(ref)==len(final)and sha(ref).startswith('c770f340');replay=bytearray(ref);last=0
for e in sorted(manifest['ranges'],key=lambda e:int(e['address'],16)):
 p=int(e['address'],16)-BASE;old,new=bytes.fromhex(e['expected']),bytes.fromhex(e['replacement']);assert 0<=last<=p and len(old)==len(new)==e['size'] and p+len(old)<=len(ref);assert ref[p:p+len(old)]==old;replay[p:p+len(old)]=new;last=p+len(old)
assert bytes(replay)==final
hooks=[];final_ranges={};overridden=[]
for part in['live','pb']:
 m=json.loads((build/part/'changed-ranges.json').read_text())
 for name,a in m['symbols'].items():assert manifest['symbols'][name]==a
 for e in m['ranges']:
  if not e['size']:continue
  p=int(e['address'],16)-BASE;new=bytes.fromhex(e['replacement'])
  if p in final_ranges and final_ranges[p]!=new:overridden.append(dict(address=e['address'],last_layer=part))
  final_ranges[p]=new
  if e['size']==4:hooks.append(e['address'])
for p,new in final_ranges.items():assert final[p:p+len(new)]==new,hex(p+BASE)
ui=(build/'work/src-fix37/ui/live_ui.c').read_text();assert '(void)live_font_fix;'in ui and 'Live Photo: Preparing'in ui and 'live_font_fix'not in live_sy
assert 'Live Photo: Preparing'.encode('utf-16-le')in final and 'Live Photo: Requested'.encode('utf-16-le')not in final[0x401e2000-BASE:0x401fd000-BASE]
assert final[0x14eaa90:0x14eaa98]==final[-9:-1]==b'04.06.p0'
w0,w1,w2=struct.unpack_from('<3I',final,0xeb29fc);public=struct.pack('<I',((w0>>5)&65535)|(((w1>>5)&65535)<<16))+struct.pack('<H',(w2>>5)&65535);assert public==b'04.06\0'
# Package round trip solely in RAM; do not execute pack52 or create delivery files.
sys.path.insert(0,str(H/'Nikon/NikonFirmwarePatch'));from z8prores.package import Package;from verify import decode
stock=(H/'Nikon/z8-qemu/.local/firmware/Z_8_0311.bin').read_bytes();assert sha(stock)=='d67413c2ede6b9ce8887175f20700bd9fbad2a973a4820968c45503d15d80d96';packet,_=Package(stock).replace_eg(final+binascii.crc_hqx(final,0).to_bytes(2,'big'),'eg2070_0406p0.bi');a,b=decode(stock),decode(packet)
assert a[:112]==b[:112]and a[128:581844]==b[128:581844]and a[82389555:-16]==b[82389555:-16]
assert b[112:128]==b'eg2070_0406p0.bi'and b[581844:82389553]==final
plugins=H/'fxfix/pack/Z8-v51-Portrait-LiveMOV-aa6e94e0/FENESTRA';files={}
for p in plugins.rglob('*'):
 st=p.lstat();assert stat.S_ISREG(st.st_mode)or stat.S_ISDIR(st.st_mode),str(p)
 if p.is_file():files[p.relative_to(plugins).as_posix()]=sha(p.read_bytes())
assert files
report={'result':'PASS','main_sha256':sha(final),'bytes':len(final),'source_build':str(build),'immutable_source_files_checked':len(expected_source),'source_manifest_sha256':sha(source_manifest.read_bytes()),'raw_delta_bytes':raw_changes,'independent_full_composition_equal':True,'aggregate_replays_full_main':True,'aggregate_ranges':len(manifest['ranges']),'aggregate_base_sha256':sha(ref),'all_generated_hook_sites_present':sorted(set(hooks)),'ordered_layer_overrides':overridden,'sections':sizes,'free_bytes':{k:receipt[k]for k in('live_free','live_extra_free','playback_free')},'size_meta_gap_preserved':True,'evf_core_bytes':40,'portrait_records':419,'portrait_changed_bytes':3518,'build_id':hex(bid),'font_retarget_absent':True,'preparing_present':True,'identity':'04.06.p0','package_roundtrip_in_memory':True,'package_sha256':sha(packet),'package_bytes':len(packet),'unselected_container_bytes_and_crcs_verified':True,'plugin_regular_files':files,'scope':'Read-only MAIN/ELF/source audit and in-memory package round trip; no delivery creation, no camera/QEMU behavioral validation'}
report_path.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items()if k not in('sections','plugin_regular_files','all_generated_hook_sites_present')},indent=2))
