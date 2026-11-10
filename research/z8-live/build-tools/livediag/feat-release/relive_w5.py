#!/usr/bin/env python3
"""Live v4 + photo-mode audio (audio/*.c). Rebuild the live-a-v4 payload (same compile/link recipe as live-a-v4/build.py) and
splice it into an existing merged MAIN, replacing only the Live layer's recorded ranges.

usage: relive.py SRC_DIR MERGED_MAIN MANIFEST OUT_DIR [--check]
  --check: only compare the rebuilt payload/hooks with MERGED_MAIN (reproducibility)."""
import hashlib, json, struct, subprocess, sys
from pathlib import Path

compile_only = '--compile-only' in sys.argv
args = [a for a in sys.argv[1:] if not a.startswith('--')]
src, main_path, manifest_path, out = map(Path, args[:4])
check = '--check' in sys.argv
LOAD = 0x40160000; START = 0x401e2000; LIMIT = 0x401fff80
sha = lambda b: hashlib.sha256(b).hexdigest()
main = b'' if compile_only else main_path.read_bytes()
manifest = {} if compile_only else json.loads(manifest_path.read_text())
SITES = {0x403db694: ('live_ui_menu_page', False), 0x4032e130: ('live_ui_menu_text', False),
         0x40d60ec8: ('live_ui_system_dispatch', False), 0x40fdc9c8: ('live_press_entry', False),
         0x40e90838: ('live_irq_entry', False), 0x40c26804: ('live_start_entry', False),
         0x40c26818: ('live_stop_entry', False), 0x40c26828: ('live_hold_entry', False),
         0x40e8b328: ('live_pool_wrap', True), 0x408b7c1c: ('live_closed_entry', False),
         0x406cd6a0: ('live_finalize_wrap', True), 0x406cd75c: ('live_event_wrap', True),
         0x40ea3638: ('live_writer_wrap', True), 0x406cd468: ('live_open_wrap', True),
         0x402f2d58: ('live_w5_bringup_entry', False)}
W5_SITE, W5_ORIG = 0x402f2d58, bytes.fromhex('ff0302d1')   # sub sp,sp,#0x80
ranges = {int(r['address'], 16): r for r in manifest.get('ranges', [])}
base_word = lambda site: struct.unpack('<I', bytes.fromhex(ranges[site]['expected']))[0]

def target(site):
    w = base_word(site); assert w >> 26 in (5, 37), hex(w)
    return site + ((((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4)

out.mkdir(parents=True, exist_ok=True); obj = out / 'objects'; obj.mkdir(exist_ok=True)
names = ['runtime.c', 'hooks.S', 'capture/live_capture.c', 'capture/native_capture.c', 'ui/live_ui.c',
         'codec/live_w5enc.c', 'codec/live_h264_idr.c', 'codec/live_mov.c', 'codec/live_jpeg.c', 'codec/live_transaction.c',
         'codec/native_files.c', 'codec/au_ring.c', 'capture/live_stream.c', 'codec/preview_writer.c',
         'audio/live_audio.c', 'audio/live_audio_mov.c', 'audio/live_audio_native.c', 'audio/live_audio_z8.c']
objects = []
for name in names:
    o = obj / (name.replace('/', '_') + '.o'); objects.append(o)
    flags = ['--target=aarch64-none-elf', '-Os', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
             '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections', '-fdata-sections']
    if name != 'codec/live_encoder.c': flags += ['-mgeneral-regs-only', '-mstrict-align']
    flags += ['-DLA_WIDTH=1024', '-DLA_HEIGHT=680', '-DLA_RING_CAPACITY=96', '-DLA_SAMPLE_INTERVAL_US=25000']
    flags += ['-DLIVE_BITRATE=8000000', '-DLIVE_IDR_US=500000', '-DLIVE_ENCODE_SPEED=9']
    flags += ['-Wall', '-Wextra', '-Werror', '-I', str(src), '-I', str(src / 'ui/core/include'), '-I', str(src / 'codec/freestanding')]
    if name == 'codec/live_encoder.c': flags += ['-D__linux__', '-Wno-unused-function', '-Wno-sign-compare']
    subprocess.run(['clang', *flags, '-c', str(src / name), '-o', str(o)], check=True)
if compile_only:
    print('compiled', len(objects)); sys.exit(0)
syms = manifest['symbols']
exports = ['fx_memset', 'fx_native_menu_build', 'fx_native_menu_link', 'fx_native_display_attach_global',
           'fx_native_display_detach', 'fx_native_display_is_current', 'fx_native_display_present', 'memcpy', 'memmove', 'memset']
defs = {k: syms[k] for k in exports}; defs['memcmp'] = syms['fx_memcmp']
defs.update(live_old_menu_page=target(0x403db694), live_old_menu_text=target(0x4032e130),
            live_old_system_dispatch=target(0x40d60ec8), live_old_movie_press=target(0x40fdc9c8),
            live_irq_resume=0x40e9083c, live_old_writer=target(0x40ea3638), live_closed_resume=0x408b7c20,
            live_start_resume=0x40c26808, live_stop_resume=0x40c2681c, live_hold_resume=0x40c2682c,
            live_old_pool=target(0x40e8b328), live_w5_bringup_resume=W5_SITE + 4)
ld = out / 'payload.ld'
ld.write_text('\n'.join(f'{k} = {v:#x};' for k, v in defs.items()) + f'''
ENTRY(live_ui_system_dispatch)
SECTIONS {{ . = {START:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }}
.data : {{ *(.data*) }} .bss : {{ *(.bss*) *(COMMON) }}
ASSERT(. <= {LIMIT:#x}, "RX padding exhausted")
ASSERT(SIZEOF(.data) == 0, "writable static data forbidden")
ASSERT(SIZEOF(.bss) == 0, "writable static state forbidden")
/DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }} }}
''')
elf = out / 'payload.elf'; blob = out / 'payload.bin'
subprocess.run(['ld.lld', '-T', str(ld), *map(str, objects), '-o', str(elf)], check=True)
subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(blob)], check=True)
nm = subprocess.check_output(['llvm-nm', '--defined-only', str(elf)], text=True)
sym = {p[2]: int(p[0], 16) for l in nm.splitlines() if len(p := l.split()) == 3}
payload = blob.read_bytes()
old_size = ranges[START]['size']
hooks = {at: struct.pack('<I', (0x94000000 if call else 0x14000000) | (((sym[s] - at) // 4) & 0x3ffffff))
         for at, (s, call) in SITES.items()}
cur = lambda at, n: main[at - LOAD:at - LOAD + n]
assert cur(W5_SITE, 4) in (W5_ORIG, hooks[W5_SITE]), cur(W5_SITE, 4).hex()
same_payload = payload == cur(START, old_size)
same_hooks = all(cur(at, 4) == h for at, h in hooks.items())
print(json.dumps(dict(payload_bytes=len(payload), recorded_bytes=old_size, payload_identical=same_payload,
                      hooks_identical=same_hooks, payload_sha256=sha(payload))))
if check: sys.exit(0 if same_payload and same_hooks else 1)
assert START + len(payload) <= LIMIT
# Region the layer may use is still all-zero in base; clear the old payload span, then write the new one.
img = bytearray(main)
span = max(old_size, len(payload))
assert all(b == 0 for b in bytes.fromhex(ranges[START]['expected']))
assert not any(img[START - LOAD + old_size:START - LOAD + span])  # growth stays inside old zero padding
img[START - LOAD:START - LOAD + span] = payload + bytes(span - len(payload))
for at, h in hooks.items(): img[at - LOAD:at - LOAD + 4] = h
# Every byte outside the Live layer must be unchanged.
live_bytes = {START - LOAD + i for i in range(span)} | {at - LOAD + i for at in SITES for i in range(4)}
diff = [i for i in range(len(main)) if main[i] != img[i]]
assert all(i in live_bytes for i in diff)
o = out / 'live-fixed.emulation-only.main'; o.write_bytes(img)
new_manifest = dict(manifest)
new_manifest['input_merged_sha256'] = sha(main); new_manifest['output_sha256'] = sha(img)
new_manifest['symbols'] = {**manifest['symbols'], **sym}
def updated(r):
    at = int(r['address'], 16)
    if at == START:
        return dict(r, size=span, expected=bytes(span).hex(), replacement=bytes(img[START - LOAD:START - LOAD + span]).hex())
    if at in SITES:
        return dict(r, replacement=hooks[at].hex())
    return r
new_manifest['ranges'] = [updated(r) for r in manifest['ranges']]
if W5_SITE not in ranges:
    new_manifest['ranges'].append(dict(address=hex(W5_SITE), size=4,
                                       purpose='live_w5_bringup_entry (Wave5 sharing)', expected=W5_ORIG.hex(),
                                       replacement=hooks[W5_SITE].hex()))
new_manifest['flashable'] = False
(out / 'changed-ranges.json').write_text(json.dumps(new_manifest, indent=1) + '\n')
print(json.dumps(dict(output=str(o), output_sha256=sha(img), changed_vs_input=len(diff))))
