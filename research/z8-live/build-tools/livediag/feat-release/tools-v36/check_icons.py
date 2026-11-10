"""Run the supplied icon fixtures on v36, with their old 112 width assertion updated to geometry macros."""
from pathlib import Path
import subprocess,shutil,json,hashlib,struct,zlib
R=Path(__file__).resolve().parent.parent;C=R/'audit-v36/icon-tests';C.mkdir(exist_ok=True)
src=R/'src-fix36';pb=R/'pb-fix36';ref=R.parent/'feat-icon/icon32-check'
fixtures=R/'tools-v36/icon-fixtures';fixtures.mkdir(exist_ok=True)
for name in ('test.c.in','pb_screen.c.in','test_alpha.c'):
 p=fixtures/name
 if not p.exists():shutil.copyfile(ref/name,p)
validator=fixtures/'scene.c'
if not validator.exists():shutil.copyfile(R.parents[2]/'_mac-migration/files/_bundle-20261102/services/fenestra-z8/src/scene.c',validator)
host=['clang','-std=c11','-Wall','-Wextra','-Werror','-ffunction-sections','-Wl,-dead_strip']
def run(name,files,includes):
 exe=C/name
 subprocess.run([*host,*(x for i in includes for x in ['-I',str(i)]),*map(str,files),'-o',str(exe)],check=True)
 return subprocess.check_output([exe])
s=(src/'ui/live_ui.c').read_text();s=s[s.index('static void icon32_compose'):s.index('void live_ui_poll(')]
s=s[:s.index('static void icon32_hide_display(')]+s[s.index('static void icon32_unlink_stale('):]
s=s.replace('icon32_orientation()','attitude').replace('FN(0x403f0b58,void *(*)(uint32_t,void *))','find_row').replace('FN(0x403f0b5c,uint32_t (*)(void *,void *))','unlink_row').replace('*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0u','photo_active')
a=s.index('    uint64_t t=0,f=0;');b=s.index('\n',a);s=s[:a]+'    uint64_t t=now;'+s[b:]
(C/'shoot.c').write_text((fixtures/'test.c.in').read_text().replace('@CODE@',s).replace('im.width==112&&im.height==44','im.width==ICON32_SHOOT_WIDTH&&im.height==ICON32_SHOOT_HEIGHT'))
frames=run('shoot',[C/'shoot.c',validator],[src/'ui',src/'ui/core/include'])
runtime=(pb/'runtime.c').read_text()
poll=runtime[runtime.index('uint32_t pb_ui_poll_wrap'):runtime.index('uint32_t pb_ui_wait_wrap')].replace('icon32_orientation()','sim_pose')
gates=runtime[runtime.index('static uint64_t pb_close_gate'):runtime.index('void pb_set_key')]
writer=runtime[runtime.index('static int ready_to_write'):runtime.index('static int playback_owner')]
(C/'pb_screen.c').write_text((fixtures/'pb_screen.c.in').read_text().replace('@GATES@',gates).replace('@WRITER@',writer).replace('@POLL@',poll).replace('im.width==112&&im.height==44','im.width==PB_BADGE_WIDTH&&im.height==PB_BADGE_HEIGHT'))
(C/'alpha.c').write_text((fixtures/'test_alpha.c').read_text().replace('#include "../icon32_alpha.h"','#include "icon32_alpha.h"'))
result={}
for name,files in [('pb_screen',[C/'pb_screen.c',pb/'ui.c',validator]),('pb_ui',[pb/'test_ui.c',pb/'ui.c',validator]),('alpha',[C/'alpha.c',pb/'ui.c']),('restore',[pb/'test_native_restore.c',pb/'native_ui.c'])]:
 result[name]=json.loads(run(name,files,[pb,pb/'core-include']))
(C/'dump.c').write_text('''#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
int main(void){FxDemoScene s;FxSceneCommand c[PB_SCENE_COMMANDS];unsigned char *p=malloc(PB_BADGE_DATA_BYTES);
if(!p||pb_scene_badge_bitmap(&s,c,p,PB_BADGE_DATA_BYTES)!=FX_OK)return 1;
size_t n=PB_BADGE_WIDTH*PB_BADGE_HEIGHT*4u;return fwrite(p+sizeof(FxSceneImage),1,n,stdout)==n?0:1;}
''')
pixels=run('dump',[C/'dump.c',pb/'ui.c'],[pb,pb/'core-include'])
def png(p):
 b=p.read_bytes();at=8;data=b''
 while at<len(b):
  n=struct.unpack_from('>I',b,at)[0];kind=b[at+4:at+8];v=b[at+8:at+8+n];at+=12+n
  if kind==b'IHDR':w,h,depth,color=struct.unpack_from('>IIBB',v);assert (depth,color)==(8,6)
  if kind==b'IDAT':data+=v
 raw=zlib.decompress(data);stride=w*4+1;assert len(raw)==stride*h and all(raw[i]==0 for i in range(0,len(raw),stride))
 return b''.join(raw[i+1:i+stride] for i in range(0,len(raw),stride))
a=R/'icons-v36/icon32-assets';assert pixels==png(a/'pb.png')
phase=[a/'shoot.png',*sorted(a.glob('shoot-busy-*.png')),a/'shoot-busy-00.png'];assert len(phase)==26
assert frames==b''.join(png(p) for p in phase)
result.update(shoot_frames=26,shoot_sha256=hashlib.sha256(frames).hexdigest(),pb_sha256=hashlib.sha256(pixels).hexdigest(),geometry='110x44; shoot center_x=54, y=98; pb x=y=24',hardware_tested=False)
(C/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
