"""Produce a separate passive diagnostic patch; does not modify production work."""
from pathlib import Path
import difflib,hashlib,json,struct
H=Path(__file__).resolve().parent;W=H/'work';O=H/'shutter-work'
names=['src-fix37/runtime.c','src-fix37/runtime.h','src-fix37/hooks.S','src-fix37/ui/live_ui.c']
code=r'''
/* Passive shutter evidence: RAM reads + existing crumbs only. Native sound
 * decisions and arguments are untouched; card I/O stays in log_step's worker.
 * EV60 a: point/incoming/prepare-state/fire-state (four bytes).
 * b: driver/service/sound-running/stock-mute/inhibit/prepared/capture/active. */
__attribute__((section(".live_extra"),noinline))
void live_shutter_diag(uint32_t point,uint32_t incoming){
 struct live_runtime *r=state();if(!r)return;
 const volatile uint8_t *sound=(const volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc7008);
 uint32_t a=(point&255u)|((incoming&255u)<<8)
  |((*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa0292be18)&255u)<<16)
  |((*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa0292be1c)&255u)<<24);
 uint64_t b=(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa02d4cb48)&255u)
  |((uint64_t)(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa031f8d28)&255u)<<8)
  |((uint64_t)sound[8]<<16)|((uint64_t)sound[0]<<24)|((uint64_t)sound[14]<<32)
  |((uint64_t)*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc4f38)<<40)
  |((uint64_t)(LOAD(&r->capture.phase)&255u)<<48)|((uint64_t)(r->audio.active&255u)<<56);
 crumb(r,now_us(),60u,a,b);
}
'''
hooks=r'''
/* v52 passive wrappers at original BL sites, preserving original arguments,
 * every integer register and NZCV before tail-calling the original function. */
.section .live_extra,"ax"
.global live_shutter_prepare_entry
live_shutter_prepare_entry:
 save_all
 ldrb w1,[x0,#8]
 mov w0,#1
 bl live_shutter_diag
 restore_all
 b live_stock_shutter_prepare
.global live_shutter_fire_entry
live_shutter_fire_entry:
 save_all
 mov w1,w0
 mov w0,#2
 bl live_shutter_diag
 restore_all
 b live_stock_shutter_fire
'''
patch=[];changes=[]
for name in names:
 old=(W/name).read_text();new=old
 if name.endswith('/runtime.c'):
  marker=' __atomic_add_fetch(&r->full_presses,1,__ATOMIC_RELAXED);'
  assert old.count(marker)==1;new=new.replace(marker,' live_shutter_diag(0u,0u);\n'+marker)
  marker='void live_runtime_ui_event(uint32_t event)'
  assert new.count(marker)==1;new=new.replace(marker,code+'\n'+marker)
 elif name.endswith('/runtime.h'):
  new=new.replace('void live_press_notify(void);','void live_press_notify(void);\nvoid live_shutter_diag(uint32_t point,uint32_t incoming);')
 elif name.endswith('/hooks.S'):new+=hooks
 else:
  marker='uint32_t live_shutter_gate(uint32_t stock) {'
  assert old.count(marker)==1;new=new.replace(marker,marker+'\n    live_shutter_diag(3u,stock);')
 out=O/name;out.parent.mkdir(parents=True,exist_ok=True);out.write_text(new)
 changes.append({'path':name,'old_sha256':hashlib.sha256(old.encode()).hexdigest(),'new_sha256':hashlib.sha256(new.encode()).hexdigest()})
 patch.extend(difflib.unified_diff(old.splitlines(keepends=True),new.splitlines(keepends=True),fromfile='a/'+name,tofile='b/'+name))
(H/'shutter.patch').write_text(''.join(patch))
fw=(H.parents[1]/'feat-v50/integration-writer/v51d/q/main.main').read_bytes();sites=[]
for site,target,symbol in [(0x40f6cff0,0x40f9cccc,'live_shutter_prepare_entry'),(0x40f6e174,0x40f9cd54,'live_shutter_fire_entry')]:
 word=struct.unpack_from('<I',fw,site-0x40160000)[0]
 assert word>>26==37 and site+(((word&0x3ffffff)^0x2000000)-0x2000000)*4==target
 sites.append({'address':hex(site),'expected':struct.pack('<I',word).hex(),'symbol':symbol,'call':True,'original_target':hex(target)})
report={'firmware_sha256':hashlib.sha256(fw).hexdigest(),'patch_sha256':hashlib.sha256((H/'shutter.patch').read_bytes()).hexdigest(),'files':changes,'extra_linker_symbols':{'live_stock_shutter_prepare':'0x40f9cccc','live_stock_shutter_fire':'0x40f9cd54'},'sites':sites,'runtime_size_change':0,'event':60,'passive_only':True}
(H/'shutter-sites.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
