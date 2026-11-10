"""Link both offline icon-test payloads. Never patches a MAIN or produces an updater."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'icon32-check'
SHA = 'c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4'
LIVE = ('runtime.c hooks.S capture/live_capture.c capture/native_capture.c capture/live_stream.c '
        'ui/live_ui.c codec/live_w5enc.c codec/live_h264_idr.c codec/live_mov.c codec/live_jpeg.c '
        'codec/live_transaction.c codec/native_files.c codec/au_ring.c codec/preview_writer.c '
        'audio/live_audio.c audio/live_audio_mov.c audio/live_audio_native.c audio/live_audio_z8.c').split()
PB = 'hooks.S runtime.c preview.c preview_writer.c sync.c native_sync.c ui.c native_ui.c native_profile.c settings.c'.split()
LIVE_DEFS = ('fx_memset fx_native_menu_build fx_native_menu_link fx_native_display_attach_global '
             'fx_native_display_detach fx_native_display_is_current fx_native_display_present memcpy memmove memset memcmp '
             'live_old_menu_page live_old_menu_text live_old_system_dispatch live_old_movie_press live_old_writer '
             'live_old_pool live_irq_resume live_closed_resume live_start_resume live_stop_resume live_hold_resume '
             'live_w5_bringup_resume').split()
PB_DEFS = ('fx_memset fx_memcpy fx_memcmp fx_native_vg_create_path fx_scene_validate fx_native_menu_build '
           'fx_native_menu_link memcpy memmove memset memcmp pb_key_resume pb_system_resume pb_delete_resume '
           'pb_protect_resume pb_settings_page_resume pb_settings_text_resume pb_ui_old_poll pb_ui_old_wait '
           'pb_leave_resume pb_state_resume').split()
FLAGS = ('--target=aarch64-none-elf -march=armv8-a -Os -ffreestanding -fno-builtin -fno-stack-protector '
         '-fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections '
         '-mstrict-align -mgeneral-regs-only -Wall -Wextra -Werror').split()


def build(tmp, name, start, limit, entry, definitions, files, manifest, image, sources, publish):
    folder = sources / ('src' if name == 'shoot' else 'pb')
    work = tmp / name
    work.mkdir()
    region = next(r for r in manifest['ranges'] if int(r['address'], 16) == start)
    previous = bytes.fromhex(region['replacement'])
    offset = start - 0x40160000
    assert image[offset:offset+len(previous)] == previous
    assert not any(image[offset+len(previous):limit-0x40160000]), 'unaccounted bytes before pinned boundary'
    symbols = manifest['symbols']
    defs = {key: symbols[key] for key in definitions}
    for key, address in defs.items():
        assert 0x40160000 <= address < 0x40160000 + len(image) and address % 4 == 0, key
    script = work / 'payload.ld'
    script.write_text('\n'.join(f'{k} = {v:#x};' for k,v in defs.items()) + f'''
ENTRY({entry})
SECTIONS {{
  . = {start:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }}
  .data : {{ *(.data*) }} .bss : {{ *(.bss*) *(COMMON) }}
  ASSERT(. <= {limit:#x}, "existing payload region exceeded")
  ASSERT(SIZEOF(.bss) == 0 && SIZEOF(.data) == 0, "writable RX state")
  /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
''')
    objects = []
    for filename in files:
        obj = work / (filename.replace('/','_') + '.o')
        includes = [folder, folder/'ui/core/include', folder/'codec/freestanding'] if name=='shoot' else [folder/'core-include',folder]
        # Keep the two expanded playback UI translation units within their
        # original 40 KiB RX slot, including measured-target asset variants.
        size_flags = ['-Oz'] if name=='pb' and filename in ('native_ui.c','ui.c') else []
        subprocess.run(['clang', *FLAGS, *size_flags, *(f'-I{i}' for i in includes), '-c', folder/filename, '-o', obj],check=True)
        objects.append(obj)
    elf_path = work / 'payload.elf'
    lld = shutil.which('ld.lld')
    if lld:
        cmd = [lld, '-T', script, *objects, '-o', elf_path]
    else:
        zig = next((ROOT.parent / 'feat-rec/.recorder-venv/lib').glob('python*/site-packages/ziglang/zig'))
        cmd = [zig, 'cc', '-target', 'aarch64-freestanding-none', '-nostdlib', f'-Wl,-e,{entry}',
               '-Wl,-T,' + str(script), *objects, '-o', elf_path]
    env = {**os.environ,'ZIG_GLOBAL_CACHE_DIR':str(tmp/'zig-cache'),'ZIG_LOCAL_CACHE_DIR':str(tmp/'zig-local')}
    subprocess.run(cmd, check=True, env=env)
    with elf_path.open('rb') as f:
        elf = ELFFile(f)
        syms = list(elf.get_section_by_name('.symtab').iter_symbols())
        assert not [s.name for s in syms if s.name and s['st_shndx']=='SHN_UNDEF']
        assert not [s.name for s in elf.iter_sections() if s['sh_size'] and s['sh_flags']&1 and s['sh_flags']&2]
        alloc = [s for s in elf.iter_sections() if s['sh_size'] and s['sh_flags']&2]
        assert min(s['sh_addr'] for s in alloc) == start
        end = max(s['sh_addr'] + s['sh_size'] for s in alloc)
        assert end <= limit
        linked = {s.name:s['st_value'] for s in syms if isinstance(s['st_shndx'],int)}
        # Every existing branch into this module must still have a named entry.
        hooks = {}
        for change in manifest['ranges']:
            if change['size'] != 4:
                continue
            at = int(change['address'],16)
            op, = struct.unpack_from('<I', image, at - 0x40160000)
            if op >> 26 not in (5,37):
                continue
            target = at + (((op & 0x3ffffff) ^ 0x2000000) - 0x2000000)*4
            if not start <= target < start+len(previous):
                continue
            oldnames = [key for key,value in symbols.items() if value==target and key in linked]
            assert oldnames, f'missing hook target {at:#x}'
            # Section aliases are legitimate only if they still resolve together.
            newtargets = {linked[k] for k in oldnames}
            assert len(newtargets)==1
            new = newtargets.pop()
            assert start <= new < end and -(1<<27)<=new-at<(1<<27) and not (new-at)%4
            hooks[hex(at)] = [oldnames[0],hex(new)]
        assert hooks
    if publish:
        shutil.copyfile(elf_path, OUT/(name+'-payload.elf'))
    return dict(start=hex(start),end_exclusive=hex(end),limit=hex(limit),bytes=end-start,
                free_bytes=limit-end,source_files=len(files),externals=defs,hooks=hooks)


def main():
    image=(ROOT/'bg34.main').read_bytes()
    manifest=json.loads((ROOT.parent/'feat-rec/bg34.changed-ranges.json').read_text())
    assert hashlib.sha256(image).hexdigest()==manifest['output_sha256']==SHA
    assert 'v32: no close at the mode switch' in (OUT/'src/runtime.c').read_text()
    with tempfile.TemporaryDirectory(prefix='icon32-link-') as path:
        tmp=Path(path)
        result={'baseline':SHA,'scope':'ELFs only; updated hook targets checked, not written'}
        jobs=[('shoot',0x401e2000,0x401fe000,'live_ui_system_dispatch',LIVE_DEFS,LIVE),
              ('pb',0x40196000,0x401a0000,'pb_key_entry',PB_DEFS,PB)]
        for job in jobs:
            result[job[0]]=build(tmp,*job,manifest,image,OUT,True)
        # Test that the documented optional target fits both actual slots.
        # Discard the variant ELF; only save the exact sizes and target checks.
        custom=tmp/'custom';custom.mkdir()
        for name in ('src','pb'):
            shutil.copytree(OUT/name,custom/name)
        assets=tmp/'variants'
        subprocess.run([sys.executable,ROOT/'icon32_generate.py','--shoot-target','1,1,3,144,56',
                        '--pb-target','1,1,3,144,56','--output-dir',assets],stdout=subprocess.DEVNULL,check=True)
        shutil.copyfile(assets/'shoot.h',custom/'src/ui/icon32_shoot.h')
        shutil.copyfile(assets/'pb.h',custom/'pb/icon32_pb.h')
        work=tmp/'variant-link';work.mkdir()
        result['synthetic_target_1_1_3_144x56']={job[0]:build(work,*job,manifest,image,custom,False) for job in jobs}
    (OUT/'link-results.json').write_text(json.dumps(result,indent=2)+'\n')
    for name in ('shoot','pb'):
        r=result[name]
        custom=result['synthetic_target_1_1_3_144x56'][name]
        print(f"{name}: {r['bytes']} bytes, {r['free_bytes']} spare, {len(r['hooks'])} branch targets; "
              f"144x56 override: {custom['bytes']} bytes, {custom['free_bytes']} spare")

if __name__=='__main__':
    main()
