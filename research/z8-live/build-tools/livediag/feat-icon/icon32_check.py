#!/usr/bin/env python3
"""Reapply to disposable local copies, cross-compile and exercise actual scene code."""
# Asset-only dispatch must precede every legacy source-copy/build side effect.
import sys
if '--assets-only' in sys.argv:
 from icon32_asset_check import check_assets
 check_assets()
 raise SystemExit(0)

from pathlib import Path
import struct,subprocess,json,zlib
R=Path(__file__).resolve().parent
C=R/'icon32-check'
# Build the owner patch from the immutable inputs on every invocation. Existing
# icon32-check/src and pb are disposable copies reserved for this check.
import shutil, sys, tempfile, hashlib
source=R/'src-fix31'
pb_source=R/'pbfix/live-playback-ui-icon'
src=C/'src'
pb=C/'pb'
for rel in ('ui/live_ui.c','runtime.c'):
 shutil.copyfile(source/rel,src/rel)
for rel in ('ui.c','ui.h','test_ui.c','runtime.c','runtime.h','native_ui.c'):
 shutil.copyfile(pb_source/rel,pb/rel)
subprocess.run([sys.executable,str(R/'v32_patch.py'),str(src)],check=True)
subprocess.run([sys.executable,str(R/'icon32_patch.py'),str(src),str(pb)],check=True)
flags=['clang','--target=aarch64-none-elf','-std=c11','-Os','-ffreestanding','-fno-builtin',
       '-fno-stack-protector','-mgeneral-regs-only','-mstrict-align','-Wall','-Wextra','-Werror']
for directory,obj in [(source,'live_ui_before'),(src,'live_ui')]:
 subprocess.run([*flags,'-I',directory,'-I',directory/'ui/core/include','-I',directory/'codec/freestanding',
                 '-c',directory/'ui/live_ui.c','-o',C/(obj+'.o')],check=True)
for directory,obj in [(pb_source,'pb_ui_before'),(pb,'pb_ui')]:
 subprocess.run([*flags,'-I',directory/'core-include','-c',directory/'ui.c','-o',C/(obj+'.o')],check=True)
subprocess.run([*flags,'-I',pb,'-I',pb/'core-include','-c',C/'offsets.c','-o',C/'offsets.o'],check=True)
for name in ('runtime','native_ui'):
 subprocess.run([*flags,'-I',pb/'core-include','-I',src/'codec/freestanding','-c',pb/(name+'.c'),'-o',C/('pb_'+name+'.o')],check=True)
# Host mock uses the patched worker. Replace only hardware calls and time. The
# matching host scene validator supports CLEAR KEEP in the same pinned ABI.
s=(src/'ui/live_ui.c').read_text()
s=s[s.index('static void icon32_compose'):s.index('void live_ui_poll(')]
s=s[:s.index('static void icon32_hide_display(')]+s[s.index('static void icon32_unlink_stale('):]
s=s.replace('icon32_orientation()','attitude')
s=s.replace('FN(0x403f0b58,void *(*)(uint32_t,void *))','find_row').replace('FN(0x403f0b5c,uint32_t (*)(void *,void *))','unlink_row')
s=s.replace('*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0u','photo_active')
a=s.index('    uint64_t t=0,f=0;'); b=s.index('\n',a)
s=s[:a]+'    uint64_t t=now;'+s[b:]
(C/'test.c').write_text((C/'test.c.in').read_text().replace('@CODE@',s))
validator=R.parents[2]/'_mac-migration/files/_bundle-20261102/services/fenestra-z8/src/scene.c'
host=['clang','-std=c11','-Wall','-Wextra','-Werror','-ffunction-sections','-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections']
subprocess.run([*host,'-I',src/'ui','-I',src/'ui/core/include',C/'test.c',validator,'-o',C/'test'],check=True)
frames=subprocess.check_output([C/'test'])
with tempfile.TemporaryDirectory(dir=C) as tmp:
 pb_exe=Path(tmp)/'pbtest'
 subprocess.run([*host,'-fsanitize=address,undefined','-I',pb,'-I',pb/'core-include','-I',R,
                 C/'test_alpha.c',pb/'ui.c','-o',pb_exe],check=True)
 alpha_checks=json.loads(subprocess.check_output([pb_exe]))
 subprocess.run([*host,'-I',pb/'core-include',pb/'test_ui.c',pb/'ui.c',validator,'-o',pb_exe],check=True)
 pb_checks=json.loads(subprocess.check_output([pb_exe]))
 # Read the canonical worker output byte-for-byte, including the outline and
 # straight alpha. This uses the real playback entry point and its allocation.
 pb_dump=Path(tmp)/'pb_dump.c'
 pb_dump.write_text("""#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
int main(void){
    FxDemoScene scene;FxSceneCommand commands[PB_SCENE_COMMANDS];
    unsigned char *buffer=malloc(PB_BADGE_DATA_BYTES);
    if(!buffer || pb_scene_badge_bitmap(&scene,commands,buffer,PB_BADGE_DATA_BYTES)!=FX_OK)return 1;
    size_t bytes=PB_BADGE_WIDTH*PB_BADGE_HEIGHT*4u;
    return fwrite(buffer+sizeof(FxSceneImage),1,bytes,stdout)==bytes?0:1;
}
""")
 subprocess.run([*host,'-I',pb,'-I',pb/'core-include',pb_dump,pb/'ui.c','-o',pb_exe],check=True)
 pb_pixels=subprocess.check_output([pb_exe])

 runtime=(pb/'runtime.c').read_text()
 poll=runtime[runtime.index('uint32_t pb_ui_poll_wrap'):runtime.index('uint32_t pb_ui_wait_wrap')]
 poll=poll.replace('icon32_orientation()','sim_pose')
 gates=runtime[runtime.index('static uint64_t pb_close_gate'):runtime.index('void pb_set_key')]
 writer=runtime[runtime.index('static int ready_to_write'):runtime.index('static int playback_owner')]
 fixture=(C/'pb_screen.c.in').read_text().replace('@GATES@',gates).replace('@WRITER@',writer)
 unit=Path(tmp)/'pb_screen.c';unit.write_text(fixture.replace('@POLL@',poll))
 subprocess.run([*host,'-I',pb,'-I',pb/'core-include',unit,pb/'ui.c',validator,'-o',pb_exe],check=True)
 pb_transitions=json.loads(subprocess.check_output([pb_exe]))
 subprocess.run([*host,'-I',pb,'-I',pb/'core-include',pb/'test_native_restore.c',pb/'native_ui.c','-o',pb_exe],check=True)
 pb_restore=json.loads(subprocess.check_output([pb_exe]))
 # A separate generated asset set exercises a measured-target override without
 # changing the defaults installed in icon32-assets or in the linked payload.
 tmp=Path(tmp)
 alt=tmp/'assets'
 subprocess.run([sys.executable,R/'icon32_generate.py','--shoot-target','1,1,3,144,56',
                 '--pb-target','1,1,3,144,56','--output-dir',alt],check=True,stdout=subprocess.DEVNULL)
 shutil.copyfile(alt/'shoot.h',tmp/'icon32_shoot.h')
 alt_pb=tmp/'pb';shutil.copytree(pb,alt_pb);shutil.copyfile(alt/'pb.h',alt_pb/'icon32_pb.h')
 subprocess.run([*host,'-DICON32_TEST_TARGET','-I',tmp,'-I',src/'ui','-I',src/'ui/core/include',
                 C/'test.c',validator,'-o',pb_exe],check=True)
 assert subprocess.check_output([pb_exe])==frames
 subprocess.run([*host,'-DICON32_TEST_TARGET','-I',alt_pb,'-I',alt_pb/'core-include',unit,
                 alt_pb/'ui.c',validator,'-o',pb_exe],check=True)
 assert json.loads(subprocess.check_output([pb_exe]))==pb_transitions
# ELF64 section/symbol inspection: no third-party tools required.
def elf(path):
 b=path.read_bytes();assert b[:6]==b'\x7fELF\x02\x01'
 off=struct.unpack_from('<Q',b,40)[0];size,n,strings=struct.unpack_from('<HHH',b,58)
 sections=[struct.unpack_from('<IIQQQQIIQQ',b,off+i*size) for i in range(n)]
 def data(s):return b[s[4]:s[4]+s[5]]
 names=data(sections[strings])
 def name(tab,k):return tab[k:tab.index(0,k)].decode()
 alloc=sum(s[5] for s in sections if s[2]&2 and s[1]!=8)
 symbols={}
 for sec in sections:
  if sec[1]!=2:continue
  table=data(sections[sec[6]])
  for p in range(sec[4],sec[4]+sec[5],sec[9]):
   nm,info,other,idx,value,length=struct.unpack_from('<IBBHQQ',b,p)
   if idx and idx<len(sections):symbols[name(table,nm)]=data(sections[idx])[value:value+length]
 return alloc,symbols
subprocess.run([sys.executable,C/'viewport_test.py'],check=True)
result={'native_badge_painter':alpha_checks,'playback_host':pb_checks,'playback_transitions':pb_transitions,'playback_unlink':pb_restore,'viewport_reader':'both owners, gate, target and changing USB-event samples checked on synthetic RAM'}
for stem in ['live_ui','pb_ui']:
 before,_=elf(C/(stem+'_before.o'));after,syms=elf(C/(stem+'.o'))
 result[stem]={'before_alloc_bytes':before,'after_alloc_bytes':after,'delta':after-before}
 if stem=='live_ui':
  for key in ['live_ui_metadata','icon32_metadata']:
   result[key]=list(struct.unpack('<'+'I'*(len(syms[key])//4),syms[key]))
_,offsets=elf(C/'offsets.o')
result['pb_offsets']=list(struct.unpack('<7I',offsets['icon32_pb_offsets']))
from icon32_viewport import PB_LAYOUT_BYTES
assert result['pb_offsets'][0] in PB_LAYOUT_BYTES, result['pb_offsets']
assets=R/'icon32-assets'
phase_files=[assets/'shoot.png',*sorted(assets.glob('shoot-busy-*.png')),assets/'shoot-busy-00.png']
assert len(phase_files)==26
def read_png(p):
 b=p.read_bytes();pos=8;stream=b''
 while pos<len(b):
  n=struct.unpack_from('>I',b,pos)[0];typ=b[pos+4:pos+8];d=b[pos+8:pos+8+n];pos+=12+n
  if typ==b'IHDR':
   w,h,depth,color=struct.unpack_from('>IIBB',d);assert depth==8 and color==6
  if typ==b'IDAT':stream+=d
 w*=4
 raw=zlib.decompress(stream);assert len(raw)==(w+1)*h
 assert all(raw[i]==0 for i in range(0,len(raw),w+1))
 return b''.join(raw[i+1:i+w+1] for i in range(0,len(raw),w+1))

assert pb_pixels==read_png(assets/'pb.png')
result['playback_rgba_sha256']=hashlib.sha256(pb_pixels).hexdigest()
phase_data=[read_png(p) for p in phase_files]
assert frames==b''.join(phase_data)
static=phase_data[0]
outline=[i for i in range(0,len(static),4) if static[i]==0 and static[i+3]>0]
gaps=[i for i in range(0,len(static),4) if static[i+3]==0]
assert outline and gaps and any(static[i]==255 and static[i+3]==255 for i in range(0,len(static),4))
for data in phase_data:
 assert all(data[i:i+4]==static[i:i+4] for i in outline+gaps)
result['shooting_frame_stream_sha256']=hashlib.sha256(frames).hexdigest()
result['shooting_frames_compared']=len(phase_files)
result['target_override_check']='synthetic device/type/target 1,1,3 selects a separately rasterized 144x56 badge; unknown tuple restores 112x44 in both owners'
result['playback_epoch_check']='cancel during compose, key change, readers busy and claim preservation checked'
result['shooting_transitions']='pose-only redraw, stale unlink/retry, viewport reflow, return from another mode'
result['checks']='Shooting ready + 24 phases + wrap match RGBA PNGs. Clear gaps and black outlines stay fixed. Playback worker matches RGBA PNG. No hardware test.'
(C/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
