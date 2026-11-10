#!/usr/bin/env python3
"""Offline patch of owner-created copies. Usage: icon32_patch.py SRC_COPY PB_COPY"""
import hashlib, os, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent

def replace(t,old,new):
    if t.count(old)!=1: raise ValueError('unexpected source anchor: '+old[:90])
    return t.replace(old,new,1)

def main():
    if len(sys.argv)!=3: raise SystemExit(__doc__)
    src,pb=[Path(p).resolve() for p in sys.argv[1:]]
    for p in (src,pb):
        if not p.is_relative_to(ROOT.parent) or p in (ROOT/'src-fix31',ROOT/'pbfix/live-playback-ui-icon',ROOT/'pbfix/live-playback-ui-v28'):
            raise SystemExit('Use copies inside this folder; original inputs are protected')
    files=[src/'ui/live_ui.c',pb/'ui.c',pb/'ui.h',pb/'test_ui.c',pb/'runtime.c',pb/'runtime.h',pb/'native_ui.c']
    headers=[src/'ui/icon32_shoot.h',pb/'icon32_pb.h',src/'ui/icon32_layout.h',pb/'icon32_layout.h',src/'ui/icon32_alpha.h',pb/'icon32_alpha.h']
    for target in files+headers:
        if target.resolve()!=target or not target.is_relative_to(ROOT.parent):
            raise SystemExit('Use normal output files in the work copies: '+str(target))
    expected=INPUT_HASHES
    for p,key in zip(files,('shoot','pb','header','pb_test','runtime','runtime_header','driver')):
        if hashlib.sha256(p.read_bytes()).hexdigest()!=expected[key]: raise SystemExit('Unsupported/already patched input: '+str(p))
    t=files[0].read_text()
    t=replace(t,'#include "live_ui.h"','#include "live_ui.h"\n#include "icon32_shoot.h"\n#include "icon32_layout.h"')
    t=replace(t,'#define UI_MAGIC 0x4c505549u','#define UI_MAGIC 0x4c50554bu /* icon32 layout; cold boot required */')
    t=replace(t,'} UiState;','''    _Alignas(4) uint8_t badge_data[sizeof(FxSceneImage)+ICON32_SHOOT_MAX_PIXELS*4u];
    uint32_t icon_phase,icon_save_error,icon_retry,icon_pose;
    uint64_t icon_busy_start,icon_fallback_until;
    _Atomic uint32_t icon_blocked;uintptr_t icon_backing;
} UiState;
_Static_assert(sizeof(FxSceneImage)==24u,"image ABI");
_Static_assert(offsetof(UiState,badge_data)%4u==0u,"pixel alignment");
_Static_assert(sizeof(UiState)<=UINT32_MAX,"native allocation size");
_Static_assert(offsetof(UiState,usb)==0x4828 && offsetof(UiState,shutdown)==24,"snapshot ABI");''')
    t=replace(t,'atomic_fetch_and_explicit(&u->gate,~UINT64_C(1),memory_order_acq_rel);','atomic_store_explicit(&u->gate,(atomic_load(&u->gate)&~UINT64_C(1))+2u,memory_order_release);')
    t=replace(t,'    u->display_status=UINT32_MAX;\n}', '    u->display_status=UINT32_MAX;u->icon_retry=0u;u->icon_fallback_until=0u;\n}')
    t=replace(t,'static void hide(UiState *u) {','static void icon32_hide_display(UiState *,int);\nstatic void hide(UiState *u) {')
    t=replace(t,'fx_native_display_detach(&u->display);','icon32_hide_display(u,0);')
    start=t.index('static void indicator(');end=t.index('void live_ui_poll(',start)
    t=t[:start]+(ROOT/'icon32_indicator.c.inc').read_text()+t[end:]
    t=replace(t,'live_ui_metadata[]={UI_MAGIC,1u,','live_ui_metadata[]={UI_MAGIC,3u,')
    t += '''\n/* Locate through the owner's final ELF symbols; offsets are target-ABI generated. */
const uint32_t icon32_metadata[]={0x4933324du,2u,sizeof(UiState),
 offsetof(UiState,display),offsetof(UiState,badge_data),sizeof(((UiState *)0)->badge_data),
 offsetof(FxNativeDisplay,width),offsetof(FxNativeDisplay,height),offsetof(FxNativeDisplay,viewport),
 offsetof(FxNativeDisplay,target),offsetof(FxNativeDisplay,attached),offsetof(UiState,gate),offsetof(UiState,icon_pose)};
'''
    p=files[1].read_text();p=replace(p,'#include "live_badge.h"','#include "icon32_pb.h"')
    a=p.index('    for(uint32_t row=0u;row<PB_BADGE_HEIGHT;');b=p.index('    c[0]=command(',a)
    p=p[:a]+'''    Icon32Asset asset=icon32_pb_default();
    for(uint32_t i=0;i<PB_BADGE_WIDTH*PB_BADGE_HEIGHT;i++)icon32_pb_rgba(&asset,i,out+4u*i);
'''+p[b:]
    p=replace(p,'command(FX_SCENE_RECT,24u,24u,1u,1u,0u)','command(FX_SCENE_RECT,ICON32_PB_X,ICON32_PB_Y,1u,1u,0u)')
    p=replace(p,'command(FX_SCENE_IMAGE,24u,24u,PB_BADGE_WIDTH,PB_BADGE_HEIGHT,0u)','command(FX_SCENE_IMAGE,ICON32_PB_X,ICON32_PB_Y,ICON32_PB_DEST_WIDTH,ICON32_PB_DEST_HEIGHT,0u)')
    a=p.index('FxResult pb_scene_badge(');b=p.index('FxResult pb_scene_badge_bitmap',a)
    p=p[:a]+'''void pb_icon32_label(FxDemoScene *s,FxSceneCommand *c,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
    c[0]=command(FX_SCENE_CLEAR,FX_SCENE_KEEP,0u,0u,0u,0u);
    c[1]=command(FX_SCENE_RECT,x,y,w,h,0u);
    const char *label="LIVE";uint32_t n=2u;
    for(uint32_t i=0u;i<4u&&h>=11u&&9u+6u*i<=w;i++)
        c[n++]=command(FX_SCENE_GLYPH,x+4u+i*6u,y+4u,label[i],1u,0xffffffu);
    scene(s,c,n,0,0u,0u);s->capacity=6u;
}
FxResult pb_scene_badge(FxDemoScene *s,FxSceneCommand c[PB_SCENE_COMMANDS]) {
    if(!s||!c)return FX_ARGUMENT;
    pb_icon32_label(s,c,ICON32_PB_X,ICON32_PB_Y,ICON32_PB_DEST_WIDTH,ICON32_PB_DEST_HEIGHT);
    return FX_OK;
}
'''+p[b:]
    h=files[2].read_text();h=replace(h,'#define PB_BADGE_WIDTH 112u\n#define PB_BADGE_HEIGHT 44u','#include "icon32_pb.h"\n#define PB_BADGE_WIDTH ICON32_PB_WIDTH\n#define PB_BADGE_HEIGHT ICON32_PB_HEIGHT')
    test=files[3].read_text()
    a=test.index('    assert(badge.width==')
    b=test.index('    memset(data,0xa5',a)
    test=test[:a]+'''    assert(badge.width==PB_BADGE_WIDTH && badge.height==PB_BADGE_HEIGHT && badge.stride==4u*PB_BADGE_WIDTH);
    assert(cmd[2].x==ICON32_PB_X && cmd[2].y==ICON32_PB_Y);
    assert(cmd[2].glyph==ICON32_PB_DEST_WIDTH && cmd[2].scale==ICON32_PB_DEST_HEIGHT);
    for(unsigned i=0;i<PB_BADGE_WIDTH*PB_BADGE_HEIGHT;i++) {
        const uint8_t *p=data+sizeof(badge)+i*4u;
        Icon32Asset asset=icon32_pb_default();uint8_t rgba[4];
        icon32_pb_rgba(&asset,i,rgba);assert(memcmp(p,rgba,4u)==0);
    }
'''+test[b:]
    test=replace(test,'badge_white_pixels'+r'\"'+':848','badge_pixels_match'+r'\"'+':true')
    # The host color fixtures explicitly fill a 320×212 frame. The original
    # macro now names the maximum accepted 1024×682 preview (and reads posture
    # RAM on that target path). Pass the fixture's actual length on successes.
    old_success='frame,PB_PREVIEW_FRAME_BYTES)==FX_OK'
    if test.count(old_success)!=4: raise ValueError('unexpected playback color fixtures')
    test=test.replace(old_success,'frame,320u*212u*3u/2u)==FX_OK')
    # Read all payloads before replacing any files. Atomic replacement also
    # isolates protected sources when the build copies were made with hard links.
    runtime=files[4].read_text()
    runtime=replace(runtime,'#include "ui.h"','#include "ui.h"\n#include "icon32_layout.h"')
    # Keep all playback masks in ui.c. A second translation unit referencing
    # the same static asset header duplicates every override in the small RX slot.
    p += '\n#include "runtime.h"\n#include "icon32_layout.h"\n#include "fenestra_memory.h"\n'
    p += (ROOT/'icon32_pb.c.inc').read_text()
    runtime=replace(runtime,' uint32_t request=LOAD(&r->render_request);\n /* MCU',""" uint32_t request=LOAD(&r->render_request);
 uint32_t pose=0u;
 if(r->display.display.attached||request==1)pose=icon32_orientation();
 /* MCU""")
    a=runtime.index(' if(request==0&&r->display.display.attached')
    b=runtime.index(' if((request!=1',a)
    runtime=runtime[:a]+""" if(request==0&&r->display.display.attached&&playback_owner()) {
  uint32_t idle=0;
  if(!atomic_compare_exchange_strong_explicit(&r->render_request,&idle,3u,memory_order_acq_rel,memory_order_relaxed))return native_result;
  if(!(LOAD(&r->gate)&1)||!pb_display_is_current(&r->display))request=2;
  else if(r->display.display.scene==&r->badge_scene&&(pose!=r->badge_pose||LOAD(&r->badge_blocked)))request=1;
  else {STORE(&r->render_request,0);return native_result;}
 } else"""+runtime[b:]
    runtime=replace(runtime,'r->scene.data&&(*(uint32_t*)r->scene.data==320||*(uint32_t*)r->scene.data==1024)',
                    'r->scene.data&&r->display.display.scene!=&r->badge_scene')
    runtime=replace(runtime,'  if(result==FX_OK){r->ui_presented++;','  if(result==FX_OK){r->badge_pose=pose;r->ui_presented++;')
    runtime=replace(runtime,'  pb_display_detach(&r->display);r->ui_detached++;\n }','  atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_acq_rel);\n  pb_display_detach(&r->display);r->ui_detached++;\n }')
    runtime=replace(runtime,'PB_SCENE_DATA_BYTES)==FX_OK)present(r);','PB_SCENE_DATA_BYTES)==FX_OK)present(r);\n    else if(pb_scene_badge(&r->scene,r->commands)==FX_OK)present(r);\n    else STORE(&r->render_request,0);')
    # MCU, UI owner and writer all invalidate the very same generation.
    import re
    runtime,count=re.subn(r'atomic_fetch_and_explicit\(&r->gate,~UINT64_C\(1\),memory_order_(?:release|acq_rel)\)', 'pb_close_gate(r,0)',runtime)
    if count!=9: raise ValueError(f'unexpected playback gate sites: {count}')
    a=runtime.index('void pb_set_key(')
    runtime=runtime[:a]+(ROOT/'icon32_pb_gate.c.inc').read_text()+runtime[a:]
    a=runtime.index('static int ready_to_write(');b=runtime.index('static int playback_owner',a)
    runtime=runtime[:a]+(ROOT/'icon32_pb_writer.c.inc').read_text()+runtime[b:]
    runtime=replace(runtime,'if(request!=3)STORE(&r->render_request,r->display.display.attached?2:0);',
        'while(request!=3u&&!atomic_compare_exchange_weak_explicit(&r->render_request,&request,\n  r->display.display.attached?2u:0u,memory_order_acq_rel,memory_order_relaxed)){}')
    runtime=replace(runtime,' STORE(&r->render_request,0);\n}\nvoid pb_ui_state_notify', ' pb_clear_pending(r);\n}\nvoid pb_ui_state_notify')
    runtime=replace(runtime,' if(r->display.display.attached&&(!(LOAD(&r->gate)&1)||LOAD(&r->ui_leaving))){',
        ' if(r->display.display.attached&&(LOAD(&r->ui_leaving)||(!(LOAD(&r->gate)&1)&&LOAD(&r->render_request)!=4u))){')
    runtime=replace(runtime,'  if(LOAD(&r->render_request)!=4)STORE(&r->render_request,0);','  pb_clear_pending(r);')
    runtime=replace(runtime,'if(!ready_to_write(r))','if(!ready_to_write(r,r->preview.epoch))')
    runtime=replace(runtime,'if(present(r)){r->frames++;','if(present(r,r->preview.epoch)){r->frames++;')
    runtime=replace(runtime,'&&ready_to_write(r))','&&ready_to_write(r,key))')
    runtime=runtime.replace('==FX_OK)present(r);','==FX_OK)present(r,key);')
    rh=replace(files[5].read_text(),'} PbRuntime;',"""    /* UI-owned projection; prior field offsets stay unchanged. */
    FxDemoScene badge_scene;
    FxSceneCommand badge_commands[6];
    uint32_t badge_pose;_Atomic uint32_t badge_blocked;uintptr_t badge_backing;
    uint64_t compose_epoch;
} PbRuntime;""")
    driver=replace(files[6].read_text(),'#include "native_ui.h"','#include "native_ui.h"\nextern const FxDemoScene *pb_icon32_scene(FxNativeDisplay *,const FxDemoScene *);')
    driver=replace(driver,'#include "fenestra_memory.h"','#include "fenestra_memory.h"\n#include "icon32_alpha.h"')
    # Only the projected badge (an owned UI scene) uses RGBA replacement.
    # Frame playback and native detach retain their original graphics contracts.
    driver=replace(driver,'#include "icon32_alpha.h"', '#include "icon32_alpha.h"\n#include "runtime.h"\nextern void pb_icon32_label(FxDemoScene *,FxSceneCommand *,uint32_t,uint32_t,uint32_t,uint32_t);')
    driver=replace(driver,'static uint32_t draw_body(','''/* A projected badge and its display are two fields of one retained owner.
 * Test field addresses before dereferencing an enclosing runtime. A page/LVP
 * scene follows the generic path, including standalone PbNativeDisplay users. */
static PbRuntime *badge_owner(const FxNativeDisplay *d) {
    uintptr_t base=(uintptr_t)d - offsetof(PbRuntime,display.display);
    return base+offsetof(PbRuntime,badge_scene)==(uintptr_t)d->scene ? (PbRuntime *)base : 0;
}
static uint32_t draw_body(''')
    driver=replace(driver,'    if(!d->scene->count) return 1u;',"""    if(!d->scene->count) return 1u;
    PbRuntime *badge=badge_owner(d);
    if(badge) {
        d->error=0;
        uint32_t result=icon32_paint(d,&pb_native_display_profile,0,&badge->badge_backing);
        if(!result && d->error && d->scene->data) {
            FxDemoScene text;
            FxSceneCommand commands[6];
            const FxSceneCommand *b=&d->scene->commands[1];
            pb_icon32_label(&text,commands,b->x,b->y,b->glyph,b->scale);
            result=icon32_paint_scene(d,&pb_native_display_profile,0,&badge->badge_backing,&text);
        }
        atomic_store_explicit(&badge->badge_blocked,result==2u,memory_order_release);
        if(result==2u){d->callbacks++;return 1;}
        return result;
    }""")
    driver=replace(driver,'    uint16_t *rect=(uint16_t *)(void *)(d->object+0x10u);','    d->scene=pb_icon32_scene(d,scene);\n    uint16_t *rect=(uint16_t *)(void *)(d->object+0x10u);')
    driver=replace(driver,'    d->scene=scene;\n    d->object','    d->scene=pb_icon32_scene(d,scene);\n    d->object')
    driver=replace(driver,'atomic_fetch_add_explicit(&d->readers,1u,memory_order_acquire);',
                   'atomic_fetch_add_explicit(&d->readers,1u,memory_order_seq_cst);')
    driver=replace(driver,'if(atomic_load_explicit(d->gate,memory_order_acquire)==ticket',
                   'if(atomic_load_explicit(d->gate,memory_order_seq_cst)==ticket')
    # Detach can run during a cancelled worker composition. Its full-backing
    # paint does not need the worker's mutable scene descriptor.
    begin=driver.index('    uint32_t left=x*d->width/d->scene->width')
    end=driver.index('    if(right<=left',begin)
    geometry=driver[begin:end]
    body=driver.index('static void rectangle(')
    close=driver.index('\n}',end)+2
    new='static void paint_backing(FxNativeDisplay *d,uint32_t left,uint32_t top,uint32_t right,uint32_t bottom,uint32_t rgb) {\n'+driver[end:close]
    driver=driver[:body]+new+'\n'+driver[body:begin]+geometry+'    paint_backing(d,left,top,right,bottom,rgb);\n}'+driver[close:]
    driver=replace(driver,'            rectangle(d,0u,0u,d->scene->width,d->scene->height,0u);',
                   '            paint_backing(d,0u,0u,d->width,d->height,0u);')
    driver=replace(driver,'        uint32_t owns_target=!d->global_overlay || is_current(d);','        PbRuntime *badge=badge_owner(d);\n        uint32_t owns_target=!d->global_overlay || (badge ? icon32_owner(d,&pb_native_display_profile) : is_current(d));')
    driver=replace(driver,'if(d->global_overlay)owns_target=owns_target && is_current(d);', '''uint32_t viewport_current=1;
        if(d->global_overlay) {
            viewport_current=is_current(d);
            owns_target=owns_target && (badge ? icon32_owner(d,&pb_native_display_profile) : viewport_current);
        }''')
    driver=replace(driver,'            ++account(d)->skipped_owner;', '            if(badge)badge->badge_backing=0;\n            ++account(d)->skipped_owner;')
    driver=replace(driver,'        bind(d);\n        const uint16_t rect[4]','''        if(d->global_overlay && badge) {
            uint32_t previously_painted=badge->badge_backing!=0;
            uint32_t painted=icon32_paint(d,&pb_native_display_profile,1,&badge->badge_backing);
            badge->badge_backing=0;
            uint32_t empty=!FN(native_list_count,uint32_t (*)(void *))((uint8_t *)d->surface+0xc8u);
            if(empty && (painted==1u || (!previously_painted && viewport_current))) {
                FN(gui_set_visible,void (*)(void *,uint32_t))(d->surface,d->original_visible);
                FN(native_graphics_target_enable,void (*)(uint32_t,uint32_t,uint32_t,uint32_t))
                    (d->target[0],d->target[1],d->target[2],d->target_was_enabled);
            }
            unlock();
            if(viewport_current && (empty || painted==1u))FN(native_menu_compositor_submit,void (*)(void *,uint32_t,uint32_t))
                ((void *)(uintptr_t)pb_native_display_profile.native_menu_progress,3u,7u);
            return;
        }
        bind(d);
        const uint16_t rect[4]''')
    outputs=[data.encode() for data in (t,p,h,test,runtime,rh,driver)]
    outputs+=[(ROOT/'icon32-assets'/name).read_bytes() for name in ('shoot.h','pb.h')]
    outputs += [(ROOT/'icon32_layout.h').read_bytes()]*2+[(ROOT/'icon32_alpha.h').read_bytes()]*2
    staged=[]
    try:
        for target,data in zip(files+headers,outputs):
            with tempfile.NamedTemporaryFile(dir=target.parent,delete=False) as f:
                staged.append((Path(f.name),target))
                f.write(data)
            os.chmod(f.name,(target.stat().st_mode&0o777) if target.exists() else 0o644)
        for tmp,target in staged: os.replace(tmp,target)
    finally:
        for tmp,_ in staged: tmp.unlink(missing_ok=True)
    print('icon32 patched source copies:',src,pb)

INPUT_HASHES = {'runtime':'f5501eef0da4205e136b96d5ad8b40a9ba59ec4135a3200fdc702e5797db6443','runtime_header':'d5f49e2b6687c8d5d09ebbd1af4ad2335d1de935f8abc00111423082d06b48a1','driver':'48f0eebb68233663c91bef60dd905af3ccb499cff34c09be4c117f88e642795c','pb_test': '8cbfc05a531c2bac88bb0f75c78ca5e9d35c2149382a2c8e6f874323f1d2d102', 'shoot': '0c97eaaf661ed3b52c53ab4233d7289470dd0e06aa7e790df914a85757ae523d', 'pb': '57c7c91cbe45f0ffee70858cd356e222efa963310493d413363594248567a1be', 'header': '0608fe501a63bb7adf2e29526fc013e46c8d083665eb618d329b6cfffe226338'} # populated from the supplied immutable inputs
if __name__=='__main__': main()
