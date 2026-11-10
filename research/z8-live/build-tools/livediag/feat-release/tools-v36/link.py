"""Link already audited objects against a verified local reference manifest, ELF only."""
from pathlib import Path
import json,subprocess,os,hashlib,io,shutil,struct
from elftools.elf.elffile import ELFFile
R=Path(__file__).resolve().parent.parent;O=R/'audit-v36'
manifest=json.loads((R.parent/'feat-rec/bg34.changed-ranges.json').read_text())
image=(R.parent/'feat-icon/bg34.main').read_bytes()
assert hashlib.sha256(image).hexdigest()==manifest['output_sha256']=='c7c6dbeea8a94064a10d47a7b9022ab26e4aa6b12ba5fb63b1ad85abac4fe4d4'
DEFS={
'src': 'fx_memset fx_native_menu_build fx_native_menu_link fx_native_display_attach_global fx_native_display_detach fx_native_display_is_current fx_native_display_present memcpy memmove memset memcmp live_old_menu_page live_old_menu_text live_old_system_dispatch live_old_movie_press live_old_writer live_old_pool live_irq_resume live_closed_resume live_start_resume live_stop_resume live_hold_resume live_w5_bringup_resume'.split(),
'pb': 'fx_memset fx_memcpy fx_memcmp fx_native_vg_create_path fx_scene_validate fx_native_menu_build fx_native_menu_link memcpy memmove memset memcmp pb_key_resume pb_system_resume pb_delete_resume pb_protect_resume pb_settings_page_resume pb_settings_text_resume pb_ui_old_poll pb_ui_old_wait pb_leave_resume pb_state_resume'.split()}
result={'reference_sha256':manifest['output_sha256'],'scope':'ELF only; no MAIN patch, no USB/QEMU'}
for group,start,limit,entry in [('src',0x401e2000,0x401fe000,'live_ui_system_dispatch'),('pb',0x40196000,0x401a0000,'pb_key_entry')]:
 out=O/group;script=out/'payload.ld';defs={k:manifest['symbols'][k] for k in DEFS[group]}
 script.write_text('\n'.join(f'{k} = {v:#x};' for k,v in defs.items())+f'''
ENTRY({entry})
SECTIONS {{ . = {start:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }}
.data : {{ *(.data*) }} .bss : {{ *(.bss*) *(COMMON) }}
ASSERT(. <= {limit:#x}, "payload overlaps pinned helper/next region")
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "writable static payload state")
/DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }} }}
''')
 objects=sorted(out.glob('*.o'));elf=out/'payload.elf'
 lld=shutil.which('ld.lld')
 if lld:cmd=[lld,'-T',str(script),*map(str,objects),'-o',str(elf)]
 else:
  zig=next((R.parent/'feat-rec/.recorder-venv/lib').glob('python*/site-packages/ziglang/zig'))
  cmd=[str(zig),'cc','-target','aarch64-freestanding-none','-nostdlib',f'-Wl,-e,{entry}','-Wl,-T,'+str(script),*map(str,objects),'-o',str(elf)]
 subprocess.run(cmd,check=True,env={**os.environ,'ZIG_GLOBAL_CACHE_DIR':str(O/'zig-cache'),'ZIG_LOCAL_CACHE_DIR':str(O/'zig-local')})
 e=ELFFile(io.BytesIO(elf.read_bytes()));syms=list(e.get_section_by_name('.symtab').iter_symbols())
 assert not [s.name for s in syms if s.name and s['st_shndx']=='SHN_UNDEF']
 assert not [s.name for s in e.iter_sections() if s['sh_size'] and s['sh_flags']&3==3]
 end=max(s['sh_addr']+s['sh_size'] for s in e.iter_sections() if s['sh_size'] and s['sh_flags']&2)
 old=next(r for r in manifest['ranges'] if int(r['address'],16)==start)
 assert image[start-0x40160000:start-0x40160000+old['size']]==bytes.fromhex(old['replacement'])
 assert not any(image[start-0x40160000+old['size']:limit-0x40160000])
 linked={s.name:s['st_value'] for s in syms if isinstance(s['st_shndx'],int)}
 hooks={}
 for change in manifest['ranges']:
  if change['size']!=4:continue
  at=int(change['address'],16);op,=struct.unpack_from('<I',image,at-0x40160000)
  if op>>26 not in (5,37):continue
  target=at+(((op&0x3ffffff)^0x2000000)-0x2000000)*4
  if not start<=target<start+old['size']:continue
  names=[k for k,v in manifest['symbols'].items() if v==target and k in linked];assert names,(hex(at),hex(target))
  new={linked[k] for k in names};assert len(new)==1;new=new.pop()
  assert start<=new<end and -(1<<27)<=new-at<(1<<27) and not (new-at)%4
  hooks[hex(at)]={'symbol':names[0],'target':hex(new)}
 assert len(hooks)==(15 if group=='src' else 10)
 result[group]={'hooks':hooks,'bytes' :end-start,'spare':limit-end,'start':hex(start),'end':hex(end),'limit':hex(limit),'command':cmd}
(O/'link-results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
