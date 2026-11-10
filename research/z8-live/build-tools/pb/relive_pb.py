#!/usr/bin/env python3
"""Rebuild the in-camera Live playback layer (codex-tasks/live-playback-ui) inside an
already composed MAIN, replacing only its declared ranges (region 0x40196000 + hook
sites), and update the overlay manifest. Adapted from live-playback-ui/build.py, which
needs the old machine's base/core paths; here every input comes from the current image
and its changed-ranges.json.

usage: relive_pb.py SRC_DIR IMAGE MANIFEST OUT_DIR [--defs JSON] [--check]
  --defs  extra link symbols (fx_glyph / fx_memmove are not in the manifest)
  --check only compare the rebuilt blob and hook words with IMAGE (reproducibility)"""
import hashlib, json, struct, subprocess, sys
from pathlib import Path

argv = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = [a for a in sys.argv[1:] if a.startswith('--')]
src, image_path, manifest_path, out = map(Path, argv[:4])
extra = json.loads(next((a[7:] for a in opts if a.startswith('--defs=')), '{}'))
check = '--check' in opts
LOAD, START, LIMIT = 0x40160000, 0x40196000, 0x401a0000
sha = lambda b: hashlib.sha256(b).hexdigest()
img = image_path.read_bytes()
man = json.loads(manifest_path.read_text())
ranges = {int(r['address'], 16): r for r in man['ranges']}
syms = man['symbols']
out.mkdir(parents=True, exist_ok=True); (out / 'objects').mkdir(exist_ok=True)

names = ['hooks.S', 'runtime.c', 'preview.c', 'preview_writer.c', 'sync.c', 'native_sync.c', 'ui.c',
         'native_ui.c', 'native_profile.c', 'settings.c']
include = src / 'core-include'
objects = []
for name in names:
    o = out / 'objects' / (name + '.o'); objects.append(o)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-mgeneral-regs-only', '-mstrict-align',
                    '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Werror', '-I', str(include),
                    '-I', str(src), '-c', str(src / name), '-o', str(o)], check=True)

core = ['fx_memset', 'fx_memcpy', 'fx_memcmp', 'fx_native_vg_create_path', 'fx_scene_validate', 'fx_glyph',
        'fx_native_menu_build', 'fx_native_menu_link', 'fx_memmove']
defs = {k: syms[k] for k in core if k in syms}
defs.update({k: int(v, 0) if isinstance(v, str) else v for k, v in extra.items()})
missing = [k for k in core if k not in defs]
assert not missing, f'missing core symbols {missing}: pass --defs'
defs.update(memcpy=defs['fx_memcpy'], memset=defs['fx_memset'], memmove=defs['fx_memmove'], memcmp=defs['fx_memcmp'])
defs.update(pb_key_resume=0x41020848, pb_system_resume=0x40d60ed0, pb_delete_resume=0x4085c428,
            pb_protect_resume=0x4085bcb4, pb_settings_page_resume=0x401cce88, pb_settings_text_resume=0x401cd3f0,
            pb_leave_resume=0x4061a728, pb_state_resume=0x40619de8)


def stock_bl_target(at):   # call sites now hold our bl; the original word is the range's `expected`
    word = struct.unpack('<I', bytes.fromhex(ranges[at]['expected']))[0]; assert word >> 26 == 37
    return at + ((((word & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4)


defs.update(pb_ui_old_poll=stock_bl_target(0x40618be0), pb_ui_old_wait=stock_bl_target(0x40618b84))
ld = out / 'payload.ld'
ld.write_text('\n'.join(f'{k} = {v:#x};' for k, v in defs.items()) + f'''
ENTRY(pb_key_entry)
SECTIONS {{ . = {START:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }}
.data : {{ *(.data*) }} .bss : {{ *(.bss*) *(COMMON) }}
ASSERT(. <= {LIMIT:#x}, "padding overflow")
ASSERT(SIZEOF(.bss) == 0 && SIZEOF(.data) == 0, "no writable RX state")
/DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }} }}
''')
elf, blobpath = out / 'payload.elf', out / 'payload.bin'
subprocess.run(['ld.lld', '-T', str(ld), *map(str, objects), '-o', str(elf)], check=True)
subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(blobpath)], check=True)
nm = subprocess.check_output(['llvm-nm', '--defined-only', str(elf)], text=True)
sym = {p[2]: int(p[0], 16) for l in nm.splitlines() if len(p := l.split()) == 3}
blob = blobpath.read_bytes()

sites = [(0x41020844, 'pb_key_entry', False), (0x40d60ecc, 'pb_system_entry', False),
         (0x4085c424, 'pb_delete_wrap', False), (0x4085bcb0, 'pb_protect_wrap', False),
         (0x401cce84, 'pb_settings_menu_page', False), (0x401cd3ec, 'pb_settings_menu_text', False),
         (0x4061a724, 'pb_leave_entry', False), (0x40619de4, 'pb_state_entry', False),
         (0x40618be0, 'pb_ui_poll_wrap', True), (0x40618b84, 'pb_ui_wait_wrap', True)]
words = {}
for at, s, call in sites:
    d = sym[s] - at; assert d % 4 == 0 and -(1 << 27) <= d < (1 << 27)
    words[at] = struct.pack('<I', (0x94000000 if call else 0x14000000) | ((d // 4) & 0x3ffffff))
old = ranges[START]
cur = lambda at, n: img[at - LOAD:at - LOAD + n]
same_blob = blob == cur(START, old['size']) and len(blob) == old['size']
same_sites = all(cur(at, 4) == w for at, w in words.items())
print(json.dumps(dict(blob_bytes=len(blob), recorded=old['size'], blob_identical=same_blob, sites_identical=same_sites,
                      blob_sha256=sha(blob))))
if check:
    (out / 'blob-current.bin').write_bytes(cur(START, max(old['size'], len(blob))))
    sys.exit(0 if same_blob and same_sites else 1)

span = max(old['size'], len(blob))
assert START + span <= LIMIT and all(b == 0 for b in bytes.fromhex(old['expected']))
assert not any(cur(START + old['size'], span - old['size']))
new = bytearray(img)
new[START - LOAD:START - LOAD + span] = blob + bytes(span - len(blob))
for at, w in words.items():
    new[at - LOAD:at - LOAD + 4] = w
owned = {START - LOAD + i for i in range(span)} | {at - LOAD + i for at in words for i in range(4)}
diff = [i for i in range(len(img)) if img[i] != new[i]]
assert all(i in owned for i in diff), [hex(LOAD + i) for i in diff if i not in owned][:8]
man2 = dict(man)
man2['ranges'] = []
for r in man['ranges']:
    at = int(r['address'], 16)
    if at == START:
        r = dict(r, size=span, expected=bytes(span).hex(), replacement=bytes(new[START - LOAD:START - LOAD + span]).hex())
    elif at in words:
        r = dict(r, replacement=words[at].hex())
    man2['ranges'].append(r)
man2['symbols'] = {**{k: v for k, v in man['symbols'].items() if not k.startswith('pb_')}, **{k: v for k, v in sym.items() if k.startswith('pb_')}}
man2['output_sha256'] = sha(new)
o = out / image_path.name
o.write_bytes(new); (out / 'changed-ranges.json').write_text(json.dumps(man2, indent=1) + '\n')
print(json.dumps(dict(output=str(o), output_sha256=sha(new), changed_vs_input=len(diff))))
