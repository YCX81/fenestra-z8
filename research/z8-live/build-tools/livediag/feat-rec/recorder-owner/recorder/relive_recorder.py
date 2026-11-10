#!/usr/bin/env python3
"""Recorder-specific relive_w5 replacement: same SRC IMAGE MANIFEST OUT interface.
Full offline rebuild, retains other layers; refuses unknown base/manifest. No device tools.
Requires clang, ld.lld, llvm-objcopy and pyelftools. Run verify_link before publication.
"""
import argparse, ast, hashlib, io, json, struct, subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from verify_link import BASE, START, LIMIT, PINNED, OLD, OLD_DEFS, patch_module, verify

def build_defines(relive):
 # Do not silently replace the owner's compile-time geometry/bitrate defaults.
 tree=ast.parse(relive.read_text());values=[]
 for n in ast.walk(tree):
  if isinstance(n,ast.JoinedStr) and any(isinstance(v,ast.Constant) and isinstance(v.value,str) and '-D' in v.value for v in n.values):
   raise ValueError('dynamic -D flags in relive: export reviewed literal flags before using this adapter')
  if isinstance(n,ast.Constant) and isinstance(n.value,str) and n.value.startswith('-D'):
   assert not any(c.isspace() for c in n.value),'one -D argument per literal required'
   if n.value.startswith(('-DLA_','-DLIVE_')) and n.value not in values:values.append(n.value)
 macros={}
 for v in values:
  key=v[2:].split('=')[0];assert key not in macros or macros[key]==v,'conflicting relive defines'
  macros[key]=v
 return values

def main(src,image,manifest,out,relive):
 root=src.resolve().parent
 assert out.resolve().is_relative_to(root),'output must stay in source workspace parent'
 assert not out.exists(),'use a fresh output directory'
 defines=build_defines(relive)
 m=patch_module(src);b=image.read_bytes();man=json.loads(manifest.read_text())
 assert hashlib.sha256(b).hexdigest()==m.SHA
 assert man['output_sha256']==m.SHA,'need the manifest corresponding to bg34.main'
 ranges={int(r['address'],16):r for r in man['ranges']}
 oldsy={k:int(v,0) if isinstance(v,str) else v for k,v in man['symbols'].items()}
 for at,(name,call) in OLD.items():
  assert name in oldsy and at in ranges
  w=struct.unpack_from('<I',b,at-BASE)[0]
  assert w>>26==(37 if call else 5)
  assert at+(((w&0x3ffffff)^0x2000000)-0x2000000)*4==oldsy[name],(hex(at),name)
 core=['fx_memset','fx_native_menu_build','fx_native_menu_link','fx_native_display_attach_global',
       'fx_native_display_detach','fx_native_display_is_current','fx_native_display_present','memcpy','memmove','memset','memcmp']
 defs={k:oldsy[k] for k in core}
 def target(at):
  w=struct.unpack('<I',bytes.fromhex(ranges[at]['expected']))[0]
  assert w>>26 in (5,37)
  return at+(((w&0x3ffffff)^0x2000000)-0x2000000)*4
 defs.update(live_old_menu_page=target(0x403db694),live_old_menu_text=target(0x4032e130),
 live_old_system_dispatch=target(0x40d60ec8),live_old_movie_press=target(0x40fdc9c8),
 live_old_writer=target(0x40ea3638),live_old_pool=target(0x40e8b328),
 live_irq_resume=0x40e9083c,live_closed_resume=0x408b7c20,live_start_resume=0x40c26808,
 live_stop_resume=0x40c2681c,live_hold_resume=0x40c2682c,live_w5_bringup_resume=0x402f2d5c)
 for name,value in OLD_DEFS.items():assert defs[name]==value,(name,'manifest disagrees with pinned bg34 target')
 sites=dict(OLD);m.extend_relive(defs,sites)
 names=['runtime.c','hooks.S','capture/live_capture.c','capture/native_capture.c','capture/live_stream.c','ui/live_ui.c',
 'codec/live_w5enc.c','codec/live_h264_idr.c','codec/live_mov.c','codec/live_jpeg.c',
 'codec/live_transaction.c','codec/native_files.c','codec/au_ring.c','codec/preview_writer.c',
 'audio/live_audio.c','audio/live_audio_mov.c','audio/live_audio_native.c','audio/live_audio_z8.c','recorder/recorder.c']
 out.mkdir();(out/'objects').mkdir();objects=[];commands=[]
 for name in names:
  obj=out/'objects'/(name.replace('/','_')+'.o');objects.append(obj)
  flags=['--target=aarch64-none-elf','-march=armv8-a','-Os','-ffreestanding','-fno-builtin','-fno-stack-protector',
  '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-fstack-usage',
  '-Wall','-Wextra','-Werror','-mstrict-align']
  if name!='hooks.S':flags+=['-mgeneral-regs-only']
  cmd=['clang',*flags,*defines,'-I',str(src),'-I',str(src/'ui/core/include'),'-I',str(src/'codec/freestanding'),'-c',str(src/name),'-o',str(obj)]
  subprocess.run(cmd,check=True);commands.append(cmd)
 layout=out/'recorder-layout.c'
 layout.write_text('#include "runtime.h"\n#include <stddef.h>\nconst unsigned fr_layout[]={sizeof(struct live_runtime),offsetof(struct live_runtime,recorder),sizeof(struct fr_state),offsetof(struct fr_state,tx),offsetof(struct live_runtime,card_epoch)};\n')
 subprocess.run(['clang','--target=aarch64-none-elf',*defines,'-I',str(src),'-I',str(src/'ui/core/include'),'-c',str(layout),'-o',str(out/'layout.o')],check=True)
 ld=out/'payload.ld'
 ld.write_text('\n'.join(f'{k} = {v:#x};' for k,v in defs.items())+f'''
ENTRY(live_ui_system_dispatch)
SECTIONS {{ . = {START:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }}
.data : {{ *(.data*) }} .bss : {{ *(.bss*) *(COMMON) }}
ASSERT(. <= {LIMIT:#x}, "recorder RX limit")
ASSERT(. <= {PINNED:#x}, "overlaps existing core helpers")
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "no writable RX state")
/DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }} }}
''')
 elf=out/'payload.elf';blob=out/'payload.bin'
 subprocess.run(['ld.lld','-T',str(ld),*map(str,objects),'-o',str(elf)],check=True)
 subprocess.run(['llvm-objcopy','-O','binary',str(elf),str(blob)],check=True)
 e=ELFFile(io.BytesIO(elf.read_bytes()));symbols={s.name:s['st_value'] for s in e.get_section_by_name('.symtab').iter_symbols() if s.name and s['st_shndx']!='SHN_UNDEF'}
 payload=blob.read_bytes();span=max(ranges[START]['size'],len(payload));assert span<=PINNED-START
 assert not any(b[START-BASE+ranges[START]['size']:START-BASE+span]),'growth would overwrite base bytes'
 result=bytearray(b);result[START-BASE:START-BASE+span]=payload+bytes(span-len(payload))
 for at,(name,call) in sites.items():
  delta=symbols[name]-at;assert delta%4==0 and -(1<<27)<=delta<1<<27
  struct.pack_into('<I',result,at-BASE,(0x94000000 if call else 0x14000000)|((delta//4)&0x3ffffff))
 final=out/'live-fixed.emulation-only.main';final.write_bytes(result)
 report=verify(image,final,elf,out/'objects/hooks.S.o',src)
 # Keep original expected bytes so downstream chain.sh retains stock provenance.
 updated=[]
 for r in man['ranges']:
  at=int(r['address'],16)
  if at==START:r=dict(r,size=span,expected=bytes(span).hex(),replacement=result[START-BASE:START-BASE+span].hex())
  elif at in sites:r=dict(r,replacement=result[at-BASE:at-BASE+4].hex())
  updated.append(r)
 for _,at,raw,_,_ in m.PROBES:
  assert at not in ranges
  updated.append(dict(address=hex(at),size=4,purpose=sites[at][0],expected=raw,replacement=result[at-BASE:at-BASE+4].hex()))
 man.update(ranges=updated,symbols={**oldsy,**symbols},output_sha256=report['final_sha256'],output=str(final),flashable=False)
 (out/'changed-ranges.json').write_text(json.dumps(man,indent=2)+'\n')
 (out/'link-check.json').write_text(json.dumps(report,indent=2)+'\n')
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('src','image','manifest','out'):p.add_argument(n,type=Path)
 p.add_argument('--relive',type=Path,required=True,help='read original relive_w5.py literal -D compile flags; never execute it')
 a=p.parse_args();main(a.src,a.image,a.manifest,a.out,a.relive)
