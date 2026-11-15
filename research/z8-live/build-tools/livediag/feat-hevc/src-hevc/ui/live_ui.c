/* Overlay for the pinned fix-20261104 core. No writable statics/RX state. */
#include "live_ui.h"
#include "icon32_shoot.h"
#include "icon32_layout.h"
#include "live_config.h"
#include "fenestra_memory.h"
#include "fenestra_native_menu.h"
#include "fenestra_native_display.h"
#include "fx_registry.h"
#include <stdatomic.h>
#include <stddef.h>
#include "runtime.h"
#define FN(a,t) ((t)(uintptr_t)(a))
#define TEXT 3154u
#define LIVE_TEXT (TEXT+23u)
#define LIVE_PAGE 0x705eu
#define UI_MAGIC 0x4c50554bu /* icon32 layout; cold boot required */
/* Original core State prefix from its actual pinned build, not newer source. */
typedef struct {
    FxNativeDescriptor setup;
    uint32_t magic,installed;
    _Atomic uint32_t activation,phase,error,shutdown,retry;
    FxNativeRow rows[47];uintptr_t events[47];
} CorePrefix;
_Static_assert(offsetof(CorePrefix,events[46])==2528u,"pinned core root slot");
_Static_assert(offsetof(CorePrefix,magic)==64u,"pinned core identity");
typedef struct {
    uint32_t magic;
    _Atomic(void *) engine;
    _Atomic uint32_t command,reload,shutdown,save_error;
    uint32_t loaded,slot,display_status;
    uint64_t generation;
    FxNativeMenu child;
    FxNativeDisplay display;
    _Atomic uint64_t gate;
    FxDemoScene scene;
    FxSceneCommand commands[16];
    uint64_t last_present_us;uint32_t presents,refreshes,present_fail,not_current;
    _Atomic uint32_t usb,usb_events,standby_kept;
    _Alignas(4) uint8_t badge_data[sizeof(FxSceneImage)+ICON32_SHOOT_MAX_PIXELS*4u];
    uint32_t icon_phase,icon_save_error,icon_retry,icon_pose;
    uint64_t icon_busy_start,icon_fallback_until;
    _Atomic uint32_t icon_blocked;uintptr_t icon_backing;
    uint32_t icon_fail_streak,icon_heals; /* v37, appended: earlier offsets unchanged */
} UiState;
_Static_assert(sizeof(FxSceneImage)==24u,"image ABI");
_Static_assert(offsetof(UiState,badge_data)%4u==0u,"pixel alignment");
_Static_assert(sizeof(UiState)<=UINT32_MAX,"native allocation size");
_Static_assert(offsetof(UiState,usb)==0x4828 && offsetof(UiState,shutdown)==24,"snapshot ABI");
extern FxNativePage *live_old_menu_page(uint32_t,uint32_t);
extern void live_old_menu_text(int32_t,int32_t,const uint16_t **,uint32_t *);
extern uint64_t live_old_system_dispatch(void *,uint32_t);
static CorePrefix *core_root(void) {
    uintptr_t root=((volatile uintptr_t *)(uintptr_t)UINT64_C(0xa032b50c8))[2];
    if(root<UINT64_C(0xa00000000)||root>UINT64_C(0xa40000000)-664u*sizeof(FxNativePage))return 0;
    FxNativePage *setup=(FxNativePage *)(root+663u*sizeof(FxNativePage));
    uintptr_t ptr=atomic_load_explicit((_Atomic uintptr_t *)&setup->descriptor,memory_order_acquire);
    if(ptr==UINT64_C(0xa028ca040)||ptr<UINT64_C(0xa00000000)||ptr>UINT64_C(0xa40000000)-sizeof(CorePrefix)||(ptr&7u))return 0;
    CorePrefix *s=(CorePrefix *)ptr;
    return s->magic==0x465a394du && s->installed && s->setup.count==46u?s:0;
}
static UiState *ui(void) {
    CorePrefix *s=core_root();if(!s)return 0;
    FxRegistry *g=fx_registry(&s->events[46],0);if(!g)return 0;
    uintptr_t p=__atomic_load_n(&g->live_ui,__ATOMIC_ACQUIRE);
    if(p<UINT64_C(0xa00000000)||p>UINT64_C(0xa40000000)-sizeof(UiState)||(p&7u))return 0;
    UiState *u=(UiState *)p;return u->magic==UI_MAGIC?u:0;
}
void *live_ui_engine_get(void) {UiState *u=ui();return u?atomic_load_explicit(&u->engine,memory_order_acquire):0;}
void live_ui_engine_set(void *engine) {UiState *u=ui();if(u)atomic_store_explicit(&u->engine,engine,memory_order_release);}
uint32_t live_ui_save_error(void) {UiState *u=ui();return u?atomic_load(&u->save_error):1u;}
static uint32_t noop(void *p) {(void)p;return 1u;}
static uint32_t request(uint32_t wanted) {
    UiState *u=ui();if(u&&!atomic_load(&u->shutdown)) {
        live_requested_set(wanted);atomic_store_explicit(&u->command,wanted,memory_order_release);
    }return 1u;
}
static uint32_t on(void *p) {(void)p;return request(1u);}
static uint32_t off(void *p) {(void)p;return request(0u);}
static void install(void) {
    CorePrefix *root=core_root();if(!root||ui())return;
    FxRegistry *g=fx_registry(&root->events[46],1);if(!g||g->live_ui)return;
    /* The playback layer may be inserting its row from another task; it holds
     * the menu lock only briefly and never blocks inside it. */
    uint32_t tries=0u;while(!fx_menu_trylock(g)){if(++tries>200u)return;FN(0x40187b00,int32_t (*)(uint32_t))(5u);}
    FxNativeMenu *core=(FxNativeMenu *)live_old_menu_page(2u,0x7000u);
    int at=fx_core_menu_slot(core,TEXT+2u);
    UiState *u=at<0?0:FN(0x40d2c408,void *(*)(uint32_t,uint32_t))((uint32_t)sizeof(*u),0u);
    if(!u){fx_menu_unlock(g);return;}
    memset(u,0,sizeof(*u));u->command=UINT32_MAX;u->display_status=UINT32_MAX;
    FxNativeMenuProfile p={&core->page,&core->rows[0],(const FxNativeEvent *)(uintptr_t)UINT64_C(0x43bff8e8),
        0x3bbu,0x285u,{0x286u,0x28bu,0x29fu,0x2aeu},0x68cu,0x7000u};
    FxNativeEntry choices[]={{TEXT+14u,0x7fb0u,0u,0u,(uintptr_t)on,0x7000u},
                            {TEXT+15u,0x7fb1u,0u,0u,(uintptr_t)off,0x7000u}};
    if(fx_native_menu_build(&u->child,&p,LIVE_PAGE,LIVE_TEXT,choices,2u,core->callbacks)!=FX_OK ||
       fx_native_menu_link(core->handlers[at+1],p.navigation,p.terminal,p.select_events,(uintptr_t)noop,LIVE_PAGE)!=FX_OK ||
       !fx_registry_claim(&g->live_ui,(uintptr_t)u)) {
        fx_menu_unlock(g);FN(0x40d2c448,void (*)(void *))(u);return;
    }
    u->scene=(FxDemoScene){720u,480u,16u,0u,u->commands,0,0u,0u};
    u->display.gate=&u->gate;u->display.exact_colors=1u;
    /* Publish last: ui() only returns objects carrying the magic. */
    __atomic_store_n(&u->magic,UI_MAGIC,__ATOMIC_RELEASE);
    fx_core_menu_commit(core,(unsigned)at,LIVE_TEXT,0x7fb2u);
    fx_menu_unlock(g);
    live_runtime_boot();
}
FxNativePage *live_ui_menu_page(uint32_t group,uint32_t id) {
    if(group==2u&&id==LIVE_PAGE){UiState *u=ui();return u?&u->child.page:0;}
    return live_old_menu_page(group,id);
}
void live_ui_menu_text(int32_t language,int32_t id,const uint16_t **text,uint32_t *bytes) {
    if(id!=(int32_t)LIVE_TEXT||!ui()){live_old_menu_text(language,id,text,bytes);return;}
#define LABEL(s) do {static const uint16_t v[]=u##s;*text=v;*bytes=(uint32_t)sizeof(v)-2u;}while(0)
    uint32_t status=live_status();
    if(live_ui_save_error()){LABEL("Live Photo (card error)");}
    else if(status&LIVE_BUSY){LABEL("Live Photo: Busy");}
    else if(status&LIVE_ERROR){LABEL("Live Photo: Unavailable");}
    else if(status&LIVE_READY){LABEL("Live Photo: Ready");}
    else if(status&LIVE_REQUESTED){LABEL("Live Photo: Requested");}
    else {LABEL("Live Photo: Off");}
#undef LABEL
}
uint64_t live_ui_system_dispatch(void *message,uint32_t sync) {
    uint32_t event=message?*(const uint32_t *)((const uint8_t *)message+32u):UINT32_MAX;
    UiState *u=ui();
    if(u&&(event==33u||event==34u)){atomic_store(&u->usb,event==33u);atomic_fetch_add(&u->usb_events,1u);}
    if(u&&(event==7u||event==24u||event==33u||event==34u))live_runtime_ui_event(event|atomic_load(&u->usb)<<8);
    /* v13: save the log now; with USB also park the boot flag (a power-off must not leave it behind) */
    if(u&&event==7u)live_runtime_flag_park((int)atomic_load(&u->usb));
    if(u&&event==7u&&atomic_load(&u->usb)) {
        atomic_fetch_add(&u->standby_kept,1u);
        return ((uint64_t (*)(void *,uint32_t))(uintptr_t)UINT64_C(0x401d6000))(message,sync);
    }
    if(u&&(event==7u||event==24u)) {
        if(event==7u)atomic_store(&u->shutdown,1u);
        else {atomic_store(&u->command,UINT32_MAX);}
        live_runtime_event(event);
    }
    uint64_t result=live_old_system_dispatch(message,sync);
    if(u&&event==24u)atomic_store(&u->reload,1u);
    if(event==19u)install();
    return result;
}
static const char *path(uint32_t slot) {return slot?"A:\\FENESTRA\\live-z8.1":"A:\\FENESTRA\\live-z8.0";}
static int read_config(uint32_t slot,uint8_t out[65],uint32_t *bytes) {
    int32_t fd=FN(0x402dd4f8,int32_t (*)(const char *,uint32_t,uint32_t))(path(slot),0u,0u);
    *bytes=0u;if(fd<0)return 0;
    uint32_t error=0u;
    while(*bytes<65u) {
        uint32_t got=0u,room=65u-*bytes;
        uint32_t r=FN(0x402dd6c8,uint32_t (*)(int32_t,void *,uint32_t,uint32_t *))(fd,out+*bytes,room,&got);
        if(r||got>room){error=1u;break;}*bytes+=got;if(!got)break;
    }
    uint32_t closed=FN(0x402dd558,uint32_t (*)(int32_t))(fd);
    return !error&&!closed;
}
static void load_config(UiState *u) {
    uint64_t best=0u;uint32_t found=0u,wanted=0u,slot=0u;
    for(uint32_t i=0u;i<2u;i++) {
        uint8_t data[65];uint32_t size=0u,r=0u;uint64_t generation=0u;
        if(read_config(i,data,&size)&&live_config_valid(data,size,&generation,&r)&&(!found||generation>best)) {
            found=1u;best=generation;wanted=r;slot=i;
        }
    }
    u->generation=best;u->slot=slot;u->loaded=1u;
    if(atomic_load_explicit(&u->command,memory_order_acquire)==UINT32_MAX)live_requested_set(wanted);
}
static int save_config(UiState *u,uint32_t wanted) {
    if(u->generation==UINT64_MAX)return 0;
    uint8_t data[64],check[65];uint32_t slot=u->slot^1u;
    live_config_make(data,u->generation+1u,wanted);
    /* Replace only our inactive config slot; the last valid slot is retained.
     * Native0x101 is exclusive creation and cannot rewrite an existing slot. */
    int32_t fd=FN(0x402dd4f8,int32_t (*)(const char *,uint32_t,uint32_t))(path(slot),0x301u,0u);
    if(fd<0)return 0;
    uint32_t written=0u;
    uint32_t error=FN(0x402dd78c,uint32_t (*)(int32_t,const void *,uint32_t,uint32_t *,void *))(fd,data,64u,&written,0);
    uint32_t closed=FN(0x402dd558,uint32_t (*)(int32_t))(fd);
    if(error||closed||written!=64u)return 0;
    uint32_t size=0u,r=0u;uint64_t generation=0u;
    if(!read_config(slot,check,&size)||!live_config_valid(check,size,&generation,&r)||generation!=u->generation+1u||r!=wanted)return 0;
    u->slot=slot;u->generation=generation;return 1;
}
static void icon32_hide_display(UiState *,int);
static void hide(UiState *u) {
    atomic_store_explicit(&u->gate,(atomic_load(&u->gate)&~UINT64_C(1))+2u,memory_order_release);
    if(u->display.attached&&!atomic_load_explicit(&u->display.readers,memory_order_acquire))icon32_hide_display(u,0);
    u->display_status=UINT32_MAX;u->icon_retry=0u;u->icon_fallback_until=0u;
}
#include "icon32_alpha.h"
/* icon32: caller is the sole UI worker; GUI callbacks borrow retained bytes. */
static void icon32_lock(void) {
    FN(0x40d2bfb4,void (*)(void *))((void *)(uintptr_t)UINT64_C(0xa032bbd68));
}
static void icon32_unlock(void) {
    FN(0x40d2bf60,void (*)(void *))((void *)(uintptr_t)UINT64_C(0xa032bbd68));
}
/* Original callback's gate contract; caller already holds the GUI mutex.
 * The pinned public is_current wrapper takes that mutex before calling
 * 0x401a29c4. Use just its under-lock predicate here. */
static uint32_t icon32_shoot_draw(void *object,void *unused) {
    (void)unused;
    FxNativeDisplay *d=object;
    uint64_t ticket=atomic_load_explicit(d->gate,memory_order_seq_cst);
    if(!(ticket&1u))return 1;
    atomic_fetch_add_explicit(&d->readers,1u,memory_order_seq_cst);
    uint32_t result=1;
    if(atomic_load_explicit(d->gate,memory_order_seq_cst)==ticket &&
       FN(0x401a29c4,uint32_t (*)(const FxNativeDisplay *))(d)) {
        UiState *u=(UiState *)((uint8_t *)d - offsetof(UiState,display));
        result=icon32_paint(d,(const FxNativeDisplayProfile *)(uintptr_t)UINT64_C(0x401dee00),0,&u->icon_backing);
        atomic_store_explicit(&u->icon_blocked,result==2u,memory_order_release);
        if(result==2u){d->callbacks++;result=1;}
    }
    atomic_fetch_sub_explicit(&d->readers,1u,memory_order_release);
    return result;
}
static void icon32_compose(UiState *u,uint32_t status,uint32_t phase) {
    const uint32_t *target=u->display.target;
    Icon32Asset asset=icon32_shoot_select(target[0],target[1],target[2]);
    const uint32_t w=asset.w,h=asset.h;
    uint32_t vw=u->display.viewport[2]-u->display.viewport[0];
    uint32_t vh=u->display.viewport[3]-u->display.viewport[1];
    Icon32Rect b=icon32_shoot_rect(vw,vh,asset.x,asset.y,asset.dw,asset.dh,ICON32_SHOOT_CENTER_X,asset.ink_mid2,asset.w);
    const uint16_t x=b.x,y=b.y;
    u->scene.width=vw;u->scene.height=vh;
    uint32_t n=0u;
    u->commands[n++]=(FxSceneCommand){FX_SCENE_CLEAR,FX_SCENE_KEEP,0u,0u,0u,0u};
    /* KEEP requires a RECT in the scene ABI. The private painter uses it only as bounds. */
    u->commands[n++]=(FxSceneCommand){FX_SCENE_RECT,x,y,b.w,b.h,0u};
    u->scene.data=u->badge_data;u->scene.data_capacity=sizeof(u->badge_data);
    u->scene.data_bytes=0u;
    if((status&LIVE_ERROR)||atomic_load(&u->save_error)||u->icon_fallback_until) {
        const char *label=atomic_load(&u->save_error)?"LIVE CARD":status&LIVE_ERROR?"LIVE WAIT":"LIVE";
        for(uint32_t i=0;label[i]&&i<12u&&4u+i*6u+5u<=b.w&&b.h>=11u;i++)
            u->commands[n++]=(FxSceneCommand){FX_SCENE_GLYPH,(uint16_t)(x+4u+i*6u),(uint16_t)(y+4u),(uint16_t)label[i],1u,0xffffffu};
    } else {
        FxSceneImage im={w,h,w*4u,FX_SCENE_RGBA8,sizeof(FxSceneImage),0u};
        for(size_t j=0;j<sizeof(im);j++)u->badge_data[j]=((const uint8_t *)&im)[j];
        uint8_t *p=u->badge_data+sizeof(im);
        for(uint32_t i=0u;i<w*h;i++) {
            icon32_shoot_rgba(&asset,i,phase,(status&LIVE_BUSY)!=0,p+4u*i);
        }
        u->scene.data_bytes=sizeof(im)+w*h*4u;
        u->commands[n++]=(FxSceneCommand){FX_SCENE_IMAGE,x,y,b.w,b.h,0u};
    }
    u->scene.count=n;
}
/* Normal exit: retire just this attachment. No full-backing clear. */
static void icon32_hide_display(UiState *u,int reflow) {
    FxNativeDisplay *d=&u->display;
    const FxNativeDisplayProfile *p=(const FxNativeDisplayProfile *)(uintptr_t)UINT64_C(0x401dee00);
    icon32_lock();
    uint32_t published=0;
    if(d->attached&&!atomic_load_explicit(&d->readers,memory_order_acquire)) {
        uint32_t valid=icon32_owner(d,p);
        if(FN(0x403f0b58,void *(*)(uint32_t,void *))(0x7fe0u,d->surface)==d->object)
            (void)FN(0x403f0b5c,uint32_t (*)(void *,void *))(d->surface,d->object);
        if(FN(0x403f0b58,void *(*)(uint32_t,void *))(0x7fe0u,d->surface)!=d->object) {
            valid=valid&&icon32_owner(d,p);
            if(valid) {
                uint32_t previously_painted=u->icon_backing!=0;
                published=icon32_paint(d,p,1,&u->icon_backing)==1u;
                if((published || (!previously_painted && FN(0x401a29c4,uint32_t (*)(const FxNativeDisplay *))(d))) &&
                   !ICON32_VG(p,native_list_count,uint32_t (*)(void *))((uint8_t *)d->surface+0xc8u)) {
                    ICON32_VG(p,gui_set_visible,void (*)(void *,uint32_t))(d->surface,d->original_visible);
                    ICON32_VG(p,native_graphics_target_enable,void (*)(uint32_t,uint32_t,uint32_t,uint32_t))
                        (d->target[0],d->target[1],d->target[2],d->target_was_enabled);
                    published=1;
                }
            }
            u->icon_backing=0;
            atomic_store_explicit(&d->attached,0u,memory_order_release);
        }
    }
    icon32_unlock();
    if(published&&!reflow)ICON32_VG(p,native_menu_compositor_submit,void (*)(void *,uint32_t,uint32_t))
        ((void *)(uintptr_t)p->native_menu_progress,3u,7u);
}
static void icon32_unlink_stale(UiState *u) {
    atomic_store_explicit(&u->gate,(atomic_load(&u->gate)&~UINT64_C(1))+2u,memory_order_release);
    icon32_hide_display(u,1);
    u->display_status=UINT32_MAX;u->icon_retry=0u;u->icon_fallback_until=0u;u->icon_fail_streak=0u;
}
/* v37: an attachment the native GUI silently dropped (menu, playback, monitor
 * rotation, mode switch) keeps failing to present. After this many consecutive
 * failures it is retired and attached again from scratch. */
#define ICON32_HEAL_AFTER 6u
static void indicator(UiState *u,uint32_t status) {
    uint32_t photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0u;
    if(!photo||!(status&LIVE_REQUESTED)||atomic_load(&u->shutdown)){hide(u);return;}
    if(u->display.attached&&!fx_native_display_is_current(&u->display)){u->not_current++;icon32_unlink_stale(u);return;}
    struct live_runtime *rt=atomic_load_explicit(&u->engine,memory_order_acquire);
    uint64_t t=0,f=0;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(t),"=r"(f));t=f?t/f*1000000u+(t%f)*1000000u/f:0;
    uint32_t every=rt?rt->stream.tune.icon_refresh_ms:0u;
    uint32_t busy=status&LIVE_BUSY;
    if(busy&&(u->display_status==UINT32_MAX||!(u->display_status&LIVE_BUSY)))
        u->icon_busy_start=t;
    uint32_t phase=busy?(uint32_t)(((t-u->icon_busy_start)/ICON32_FRAME_US)%ICON32_PHASES):0u;
    uint32_t pose=icon32_orientation();
    uint32_t saved=atomic_load(&u->save_error);
    int changed=status!=u->display_status||saved!=u->icon_save_error||pose!=u->icon_pose;
    int probe=u->icon_fallback_until&&t>=u->icon_fallback_until;
    int animate=busy&&!saved&&!(status&LIVE_ERROR)&&!u->icon_fallback_until&&phase!=u->icon_phase;
    int refresh=every&&t-u->last_present_us>=(uint64_t)every*1000u;
    if(atomic_load_explicit(&u->icon_blocked,memory_order_acquire))u->icon_retry=1u;
    if(u->display.attached&&!changed&&!animate&&!refresh&&!probe&&!u->icon_retry)return;
    if(u->icon_retry&&t-u->last_present_us<ICON32_FRAME_US)return;
    /* Advance epoch even when a previous update left the gate disabled. */
    uint64_t epoch=(atomic_load_explicit(&u->gate,memory_order_acquire)&~UINT64_C(1))+2u;
    atomic_store_explicit(&u->gate,epoch,memory_order_release);
    /* Capture the new target before laying out or enabling a draw. A gated
     * first attach cannot publish a frame with the preceding viewport. */
    FxResult r=FX_OK;
    if(!u->display.attached) {
        if(atomic_load_explicit(&u->display.readers,memory_order_acquire))return;
        if(!u->scene.count) {
            u->commands[0]=(FxSceneCommand){FX_SCENE_CLEAR,FX_SCENE_KEEP,0u,0u,0u,0u};
            u->scene.count=1u;
        }
        r=fx_native_display_attach_global(&u->display,&u->scene);
        if(r!=FX_OK) {
            u->last_present_us=t;u->present_fail++;u->icon_retry=1u;
            return;
        }
    }
    icon32_lock();
    if(atomic_load_explicit(&u->display.readers,memory_order_acquire)) {
        /* Skip: old bytes remain immutable. Old tickets cannot pass a new epoch. */
        atomic_store_explicit(&u->gate,epoch|1u,memory_order_release);
        icon32_unlock();return;
    }
    /* Gate has stayed closed since attach. Only our new base object receives
     * this callback; stock objects and the shared core keep their renderer. */
    *(uintptr_t *)(void *)(u->display.object+0x48u)=(uintptr_t)icon32_shoot_draw;
    if(probe)u->icon_fallback_until=0u;
    icon32_compose(u,status,phase);u->display.error=0u;
    atomic_store_explicit(&u->gate,epoch|1u,memory_order_release);
    icon32_unlock(); /* present/attach acquire GUI exclusion themselves. */
    {
        r=fx_native_display_present(&u->display,&u->scene);
        /* Only an attached bitmap draw with a graphics error needs a label.
         * Menu/view transitions and temporarily occupied targets retry the bitmap. */
        if(u->display.error&&u->scene.data_bytes)u->icon_fallback_until=t+2000000u;
    }
    u->last_present_us=t;
    if(r==FX_OK&&!u->display.error) {
        u->display_status=status;u->icon_save_error=saved;u->icon_phase=phase;u->icon_pose=pose;
        u->icon_retry=0u;u->icon_fail_streak=0u;u->presents++;if(refresh)u->refreshes++;
    } else {
        u->present_fail++;u->icon_retry=1u;
        if(rt)live_log(&rt->stream,t,EV_ICON,status,u->display.error);
        /* Retry on the existing attachment; a persistent failure starts over. */
        if(++u->icon_fail_streak>=ICON32_HEAL_AFTER){u->icon_heals++;icon32_unlink_stale(u);}
    }
}
void live_ui_poll(void) {
    UiState *u=ui();if(!u)return;
    if(atomic_load(&u->shutdown)){hide(u);return;}
    if(atomic_exchange_explicit(&u->reload,0u,memory_order_acq_rel))u->loaded=0u;
    if(!u->loaded)load_config(u);
    uint32_t wanted=atomic_exchange_explicit(&u->command,UINT32_MAX,memory_order_acq_rel);
    if(wanted<=1u)atomic_store(&u->save_error,save_config(u,wanted)?0u:1u);
    indicator(u,live_status());
}
/* Read-only QEMU diagnostic schema, never a mutable fixed-address mailbox. */
const uint32_t live_ui_metadata[]={UI_MAGIC,3u,(uint32_t)sizeof(UiState),
    (uint32_t)offsetof(UiState,engine),(uint32_t)offsetof(UiState,command),
    (uint32_t)offsetof(UiState,save_error),(uint32_t)offsetof(UiState,loaded),
    (uint32_t)offsetof(UiState,generation),(uint32_t)offsetof(UiState,child),
    (uint32_t)offsetof(UiState,display),(uint32_t)offsetof(UiState,display_status),
    (uint32_t)(offsetof(UiState,display)+offsetof(FxNativeDisplay,attached)),
    (uint32_t)(offsetof(UiState,display)+offsetof(FxNativeDisplay,frames)),
    (uint32_t)(offsetof(UiState,display)+offsetof(FxNativeDisplay,error))};

/* Locate through the owner's final ELF symbols; offsets are target-ABI generated. */
const uint32_t icon32_metadata[]={0x4933324du,2u,sizeof(UiState),
 offsetof(UiState,display),offsetof(UiState,badge_data),sizeof(((UiState *)0)->badge_data),
 offsetof(FxNativeDisplay,width),offsetof(FxNativeDisplay,height),offsetof(FxNativeDisplay,viewport),
 offsetof(FxNativeDisplay,target),offsetof(FxNativeDisplay,attached),offsetof(UiState,gate),offsetof(UiState,icon_pose)};
