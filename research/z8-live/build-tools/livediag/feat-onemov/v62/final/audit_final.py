"""Independent V61 -> V62 source, ELF, exact-byte, and branch-target audit."""
from pathlib import Path
from elftools.elf.elffile import ELFFile
import ast, hashlib, io, json, struct, sys

P = Path(__file__).resolve().parent
V62 = P.parent
V61 = V62.parent/'v61'
INPUT = P/'audit-inputs'
BASE = 0x40160000
sha = lambda b: hashlib.sha256(b).hexdigest()
load = lambda p: json.loads(p.read_text())
old = (V61/'final/main.main').read_bytes()
new = (P/'main.main').read_bytes()
assert sha(old) == 'db0c847b333f0a6e07222f2207435e9bf340ce1d544704f6d801ec3ecc1088e5'
assert sha(new) == '36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5'
assert len(old) == len(new) == 81807709
cur = lambda image, address, count: image[address-BASE:address-BASE+count]
receipt = load(INPUT/'remote-object-audit.json')
assert receipt['passed'] and receipt['work_source_matches_uploaded_input']
assert len(receipt['objects']) == 31
assert sum(not x['identical'] for x in receipt['objects']) == 2
assert all(x['exact_object_match'] for x in receipt['recompiled'])
assert all(x['exact_elf_match'] for x in receipt['relinked'])
assert all(x['identical'] for x in receipt['recipes'])

def inventory(root):
    return {str(p.relative_to(root)): sha(p.read_bytes()) for p in root.rglob('*') if p.is_file()}

sources = [inventory(V61/'work'), inventory(V62/'work')]
assert sources[0].keys() == sources[1].keys()
changed_sources = sorted(n for n in sources[0] if sources[0][n] != sources[1][n])
assert changed_sources == ['src-fix37/audio/live_shutter_z8.c', 'src-fix37/hooks.S']
assert sorted(receipt['source_changes']) == changed_sources
remote_input = load(INPUT/'v62-source-input.json')
assert sources[1] == remote_input == load(V62/'remote-build.json')['files']
assert sources[1]['src-fix37/audio/live_shutter_z8.c'] == '5179121322cd6f37c5eac6b2d2d2ad5b1c2b115f22f2dac05bbd56709d7edb9f'
assert sources[1]['src-fix37/hooks.S'] == 'c8e1e46f1644b07cfe43d461f52e4e4f87312536d68a2394c4cffca1f4efafbe'

def elf(path):
    data = path.read_bytes()
    e = ELFFile(io.BytesIO(data))
    assert e['e_machine'] == 'EM_AARCH64'
    sections = [s for s in e.iter_sections() if s['sh_flags'] & 2 and s['sh_size']]
    assert all(not s['sh_flags'] & 1 and s['sh_type'] != 'SHT_NOBITS' for s in sections)
    symbols = {}
    for s in e.get_section_by_name('.symtab').iter_symbols():
        if s.name and s['st_shndx'] != 'SHN_UNDEF':
            symbols.setdefault(s.name, []).append(s)
    return dict(path=str(path), sha256=sha(data), sections=sections, symbols=symbols)

live = [elf(INPUT/'v61-live.elf'), elf(V62/'build/live.elf')]
pb = [elf(INPUT/'v61-pb.elf'), elf(V62/'build/pb.elf')]
for i in range(2):
    for kind, item in [('live', live[i]), ('pb', pb[i])]:
        assert item['sha256'] == receipt['relinked'][i*2 + (kind=='pb')]['sha256']
assert pb[0]['sha256'] == pb[1]['sha256']

def sym(item, name):
    candidates = item['symbols'][name]
    values = {s['st_value'] for s in candidates}
    assert len(values) == 1, name
    return next(iter(values))

section_matches = []
for version, image, a, b in [('v61', old, live[0], pb[0]), ('v62', new, live[1], pb[1])]:
    for kind, e in [('live', a), ('pb', b)]:
        for s in e['sections']:
            data = s.data(); addr = s['sh_addr']
            assert cur(image, addr, len(data)) == data, (version, kind, s.name)
            section_matches.append(dict(version=version, kind=kind, section=s.name,
                address=hex(addr), size=len(data), sha256=sha(data), exact_elf_match=True))

def packed_region(e, start, end):
    out = bytearray(end-start); occupied = set()
    for s in e['sections']:
        a, n = s['sh_addr'], s['sh_size']
        if not start <= a < end: continue
        assert a+n <= end
        offsets = set(range(a-start, a-start+n)); assert not occupied & offsets
        occupied |= offsets; out[a-start:a-start+n] = s.data()
    assert occupied
    return bytes(out)

# Exact reconstruction includes every alignment gap and unused tail, not a
# blanket allow-list of the three reservations.
sys.path.insert(0, str(V62/'menu'))
import patch_menu_context as menu
pre_menu = (V62/'build/pre-menu.main').read_bytes()
meta = load(P/'main.json'); build = load(V62/'build/build.json')
assert sha(pre_menu) == meta['pre_menu_sha256']
patched, facts = menu.patch(pre_menu, sha(pre_menu), build)
assert sha(patched) == meta['menu_patch_sha256'] and facts == meta['menu_context']
scratch = bytearray(patched); struct.pack_into('<I', scratch, 0x401FE060-BASE, 0)
bid = struct.unpack('<I', hashlib.sha256(scratch).digest()[:4])[0] | 0x01000000
struct.pack_into('<I', scratch, 0x401FE060-BASE, bid)
assert bytes(scratch) == new and hex(bid) == meta['build_id']

regions = [('playback', 0x40196000, 0x4019F020, pb),
           ('shutter', 0x4019F020, 0x4019FF40, live),
           ('live_primary', 0x401E2000, 0x401FD000, live),
           ('live_extra', 0x401FD218, 0x401FE000, live)]
region_records = []
approved = set()
for name, start, end, pair in regions:
    for i, image in enumerate([old, new]):
        expected = bytearray(packed_region(pair[i], start, end))
        if i == 1 and name == 'live_primary':
            at = menu.HELPER-start
            assert expected[at:at+len(menu.HELPER_BYTES)] == bytes(len(menu.HELPER_BYTES))
            expected[at:at+len(menu.HELPER_BYTES)] = menu.HELPER_BYTES
        assert bytes(expected) == cur(image, start, end-start), (name, i)
    before, after = cur(old,start,end-start),cur(new,start,end-start)
    count = sum(a != b for a,b in zip(before,after))
    if name == 'playback': assert count == 0
    else: approved.update(range(start-BASE, end-BASE))
    region_records.append(dict(name=name, start=hex(start), end_exclusive=hex(end),
        old_sha256=sha(before), new_sha256=sha(after), different_bytes=count,
        reconstructed_from_verified_elf_and_zero_padding=True,
        separate_menu_helper_included=(name=='live_primary')))

def literal_assignment(path, target):
    tree = ast.parse(path.read_text())
    return next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign)
                and any(isinstance(t,ast.Name) and t.id==target for t in n.targets))

branches = []
def branch_at(address, name, call, pair, origin):
    vals=[]
    for image, item in zip([old,new],pair):
        word=struct.unpack('<I',cur(image,address,4))[0]
        assert word >> 26 == (37 if call else 5), (hex(address),hex(word))
        target=address+((((word&0x3FFFFFF)^0x2000000)-0x2000000)*4)
        assert target == sym(item,name), (hex(address),name,hex(target))
        vals.append(target)
    branches.append(dict(address=hex(address), instruction='BL' if call else 'B', symbol=name,
        old_target=hex(vals[0]), new_target=hex(vals[1]), origin=origin,
        changed=cur(old,address,4)!=cur(new,address,4), actual_elf_target_verified=True))
    approved.update(range(address-BASE,address-BASE+4))

for address,(name,call) in literal_assignment(INPUT/'relive_v37.py','SITES').items():
    branch_at(address,name,call,live,'generated Live build recipe')
for address,name,call in literal_assignment(INPUT/'relive_pb53.py','sites'):
    branch_at(address,name,call,pb,'generated playback build recipe')
for address,name,call,pair,origin in [
    (0x40F6CFF0,'live_shutter_prepare_entry',True,live,'compose_base53 prepare overlay'),
    (0x40F6E174,'live_shutter_fire_entry',True,live,'compose_base53 fire overlay'),
    (0x40E30E3C,'live_shutter_dispatch',False,live,'compose53 SFX dispatch overlay'),
    (0x401A3380,'pb_output_valid',True,pb,'core_output_patch EVF adapter'),
    (0x401A38C8,'pb_output_initialize_global',False,pb,'core_output_patch EVF adapter')]:
    branch_at(address,name,call,pair,origin)

pointers=[]
for address,name,pair in [(0x401DEEC0,'pb_output_target_enabled',pb),
                          (0x401DEEC8,'pb_output_target_enable',pb),
                          (0x401DEED8,'pb_output_backing_rectangle',pb),
                          (0x401DEF00,'live_ui_evf_submit',live)]:
    vals=[struct.unpack('<Q',cur(image,address,8))[0]for image in [old,new]]
    assert vals == [sym(e,name)for e in pair]
    pointers.append(dict(address=hex(address),symbol=name,old_target=hex(vals[0]),new_target=hex(vals[1]),
        changed=vals[0]!=vals[1],actual_elf_target_verified=True))
    approved.update(range(address-BASE,address-BASE+8))

# Separate fixed assembly overlays, inherited unchanged from V61.
fixed=[]
for address,target,name in [(0x40CF7D08,0x4019FF40,'crop_fix'),(0x407138D0,0x4019FFC0,'aspect_fix')]:
    data=cur(new,address,4); assert data == cur(old,address,4)
    w=struct.unpack('<I',data)[0]; assert w>>26 == 5
    assert address+((((w&0x3FFFFFF)^0x2000000)-0x2000000)*4) == target
    fixed.append(dict(address=hex(address),target=hex(target),symbol=name,unchanged=True))
unchanged=[]
for name,start,end in [('crop_aspect_assembly',0x4019FF40,0x401A0000),
                       ('bootguard_metadata_helpers',0x401FD000,0x401FD218)]:
    data=cur(new,start,end-start); assert data==cur(old,start,end-start)
    unchanged.append(dict(name=name,start=hex(start),end_exclusive=hex(end),sha256=sha(data),identical=True))

# The menu hook has a separately assembled, pinned target outside the ELF.
def decoded_branch(image,address):
    w=struct.unpack('<I',cur(image,address,4))[0]; assert w>>26 in (5,37)
    return w>>26,address+((((w&0x3FFFFFF)^0x2000000)-0x2000000)*4)
assert decoded_branch(old,menu.HOOK)==(37,0x401CC314)
assert decoded_branch(new,menu.HOOK)==(37,menu.HELPER)
assert decoded_branch(new,0x401FCF50)==(37,0x403DBDB4)
assert decoded_branch(new,0x401FCF80)==(5,0x401CC314)
approved.update(range(menu.HOOK-BASE,menu.HOOK-BASE+4))
approved.update(range(0x401FE060-BASE,0x401FE064-BASE))

prepare_tail = 0x401FD9C8
assert decoded_branch(old,prepare_tail)==(5,sym(live[0],'live_stock_shutter_prepare'))
assert decoded_branch(new,prepare_tail)==(5,sym(live[1],'live_shutter_prepare'))
extra_changes=[BASE+i for i in range(0x401FD218-BASE,0x401FE000-BASE)if old[i]!=new[i]]
assert extra_changes==list(range(prepare_tail,prepare_tail+4))
rodata=[next(s for s in e['sections']if s.name=='.rodata')for e in live]
assert len(rodata[0].data())==len(rodata[1].data())==5632
rdiff=[i for i,(a,b)in enumerate(zip(rodata[0].data(),rodata[1].data()))if a!=b]
rpointers=[]
for offset in sorted({i//8*8 for i in rdiff}):
    values=[struct.unpack_from('<Q',s.data(),offset)[0]for s in rodata]
    relative=[v-s['sh_addr']for v,s in zip(values,rodata)]
    assert relative[0]==relative[1] and 0<=relative[0]<5632
    rpointers.append(dict(offset=hex(offset),old_target=hex(values[0]),new_target=hex(values[1]),
        same_rodata_relative_target=hex(relative[0])))
assert len(rdiff)==8 and len(rpointers)==4

freeze=[]
for i,e in enumerate(live):
    entries=[s for s in e['symbols']['freeze'] if s['st_info']['type']=='STT_FUNC']
    assert len(entries)==1
    s=entries[0]; b=cur([old,new][i],s['st_value'],s['st_size'])
    freeze.append(dict(address=hex(s['st_value']),size=s['st_size'],sha256=sha(b)))
assert freeze[0]==freeze[1]
stream='src-fix37/capture/live_stream.c'
assert sources[0][stream]==sources[1][stream]
freeze_rule='if(count>3u&&count%3u==1u&&s->frozen[count-2].pts_us>=s->clip_press){count--;bytes-=(unsigned)s->frozen[count].bytes;s->clip_end=s->frozen[count].pts_us;}'
assert (V62/'work'/stream).read_text().count(freeze_rule)==1
stream_object=next(x for x in receipt['objects']if x['name']=='live/objects/capture_live_stream.c.o')
assert stream_object['identical']
changed=[i for i,(a,b)in enumerate(zip(old,new))if a!=b]
unknown=[i for i in changed if i not in approved]
assert not unknown, [hex(BASE+i)for i in unknown[:100]]
assert len(changed)==7545
result=dict(passed=True,baseline_main_sha256=sha(old),final_main_sha256=sha(new),bytes=len(new),
    different_bytes=len(changed),unexplained_difference_addresses=[],
    source=dict(files=len(sources[1]),changed_files=[dict(path=n,old_sha256=sources[0][n],new_sha256=sources[1][n])for n in changed_sources],
        local_matches_remote_input=True,remote_compiled_work_matches_input=True),
    compilation=dict(compiler=receipt['compiler'],linker=receipt['linker'],object_count=31,unchanged_objects=29,
        modified_units_recompiled_exactly=True,all_four_old_new_live_pb_elfs_relinked_exactly=True,
        receipt='audit-inputs/remote-object-audit.json',receipt_sha256=sha((INPUT/'remote-object-audit.json').read_bytes())),
    exact_elf_section_matches=section_matches,regions=region_records,branches=branches,pointers=pointers,
    fixed_assembly_branches=fixed,unchanged_special_regions=unchanged,
    extra_prepare_tailcall=dict(address=hex(prepare_tail),old_symbol='live_stock_shutter_prepare',
        old_target=hex(sym(live[0],'live_stock_shutter_prepare')),new_symbol='live_shutter_prepare',
        new_target=hex(sym(live[1],'live_shutter_prepare')),only_four_changed_bytes_in_extra=True),
    rodata_relocation=dict(old_base=hex(rodata[0]['sh_addr']),new_base=hex(rodata[1]['sh_addr']),
        size=5632,changed_content_bytes=8,pointers=rpointers,all_other_contents_identical=True),
    menu=dict(hook_address=hex(menu.HOOK),old_target='0x401cc314',new_target=hex(menu.HELPER),
        helper_bytes=len(menu.HELPER_BYTES),helper_sha256=sha(menu.HELPER_BYTES),
        original_current_page_call='0x403dbdb4',original_state_tailcall='0x401cc314',
        patch_script_sha256=sha((V62/'menu/patch_menu_context.py').read_bytes()),
        exact_reapplication_verified=True,changed_bytes_excluding_build_id=facts['changed_byte_count']),
    build_id=dict(address='0x401fe060',value=hex(bid),sha256_zero_then_low32_or_01000000_verified=True),
    video_freeze=dict(source=stream,source_sha256=sources[1][stream],source_unchanged=True,
        object=stream_object,linked_function=freeze[1],linked_function_identical=True,rule=freeze_rule),
    scope='Every MAIN byte compared. Changed banks exactly reconstructed from independently recompiled/relinked ELF plus zero padding and the explicit menu helper. No camera/card access; static provenance audit, not hardware behavior proof.')
(P/'binary-audit.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(dict(passed=True,different_bytes=len(changed),unexplained_bytes=len(unknown),
    branches_verified=len(branches),pointers_verified=len(pointers),playback_bank_identical=True,
    video_freeze_source_object_and_function_identical=True),indent=2))
