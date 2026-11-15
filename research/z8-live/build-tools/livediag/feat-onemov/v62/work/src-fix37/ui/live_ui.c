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
#include "evf_diag.h"
#define FN(a,t) ((t)(uintptr_t)(a))
#define TEXT 3154u
#define LIVE_TEXT (TEXT+23u)
#define LIVE_PAGE 0x705eu
#define SOUND_TEXT (TEXT+9u) /* unused core text id: Live shutter-sound row */
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
    _Atomic uint32_t mute_cmd; /* v39: UINT32_MAX none, else wanted shutter-sound mute */
    uint32_t mute,saved_requested;
    _Atomic uint32_t mute_live,shutter_seen,shutter_suppressed; /* v39: gate state + USB counters */
    uint32_t target_lost; /* v44: overlay graphics target found disabled -> re-attached */
    uint64_t load_first_us;uint32_t load_waits,load_retries; /* v45 */
    struct evf_diag evf_diag; /* v53 passive: no earlier offset changes */
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
/* Called only while an explicit stock codec handoff is pending. Photo REC
 * itself keeps its speaker; a normal playback-view hold must not call this. */
__attribute__((section(".live_extra"),noinline))
int live_ui_pb_audio_handoff(void) {
    CorePrefix *s=core_root();if(!s)return 1;
    FxRegistry *g=fx_registry(&s->events[46],0);if(!g)return 1;
    uintptr_t fn=__atomic_load_n(&g->spare[0],__ATOMIC_ACQUIRE);
    if(!fn)return 1; /* playback worker has not been published */
    if((fn&3u)||fn<UINT64_C(0x40196000)||fn>=UINT64_C(0x401a0000))return 0;
    return ((int (*)(void))fn)();
}
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
void live_shutter_mute_set(uint32_t mute) {UiState *u=ui();if(u)atomic_store(&u->mute_live,mute?1u:0u);}
uint32_t live_shutter_mute_get(void) {UiState *u=ui();return u?atomic_load(&u->mute_live):0u;}
/* v39: stock shutter-sound task, at its mute check (hooks.S live_shutter_entry).
 * Mute only while Live is requested in photo mode; the stock silent path then
 * runs unchanged. Lock-free, no IO. */
uint32_t live_shutter_gate(uint32_t stock) {
    live_shutter_diag(3u,stock);
    UiState *u=ui();if(!u)return stock;
    atomic_fetch_add(&u->shutter_seen,1u);
    if(stock)return stock;
    int photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0;
    if(!photo||!atomic_load(&u->mute_live)||atomic_load(&u->shutdown)||!(live_status()&LIVE_REQUESTED))return 0u;
    atomic_fetch_add(&u->shutter_suppressed,1u);return 1u;
}
/* v39: toggle shutter-sound mute (applies while Live is requested); saved with the Live config. */
static uint32_t sound(void *p) {
    (void)p;UiState *u=ui();if(u&&!atomic_load(&u->shutdown)) {
        uint32_t m=live_shutter_mute_get()?0u:1u;live_shutter_mute_set(m);
        atomic_store_explicit(&u->mute_cmd,m,memory_order_release);
    }return 1u;
}
/* v40: Nikon's CJK menu faces (class 0: ja 0x0d, zh-CN 0x02, zh-TW 0x00,
 * ko 0x16) draw ASCII half-width and use some ASCII codes for icons: in
 * Japanese/Chinese 'q' is a pictogram, so "Requested" lost a letter.
 * Fenestra's text bank gives every core/Live id class 0. Install a copy of the
 * stock class table with one extra class whose CJK entries use the English
 * Latin face (class 0, language 1); all other languages keep class 0. Then
 * point the bank's appended ids at it. Stock ids and stock tables are not
 * modified; the published table is never freed (stock readers may hold it). */
#define FONT_CLASSES 18u
#define FONT_LANGS 36u
#define FONT_MAGIC 0x544e4f46u
typedef struct {
    const void *previous;uint32_t stock,capacity,languages,bytes,retained_bytes;
    uint8_t font_default,attribute_default;uint8_t *fonts;uintptr_t *attributes;
} TextBankPrefix;
_Static_assert(offsetof(TextBankPrefix,font_default)==28u&&offsetof(TextBankPrefix,fonts)==32u&&
               offsetof(TextBankPrefix,attributes)==40u,"pinned text bank layout");
typedef struct {const uint8_t *classes[FONT_CLASSES+1u];uint64_t entries[FONT_LANGS];uint32_t magic;} FontClassTable;
static int font_lang_cjk(uint32_t l) {return l==0u||l==8u||l==9u||l==10u;}
static void live_font_fix(void) {
    uint8_t *fonts=*(uint8_t *volatile *)(uintptr_t)UINT64_C(0xa02f2ac40);
    uintptr_t attrs=*(volatile uintptr_t *)(uintptr_t)UINT64_C(0xa02f2ac50);
    const uint8_t *const *volatile *slot=(const uint8_t *const *volatile *)(uintptr_t)UINT64_C(0xa02f2ac28);
    const uint8_t *const *old=*slot;
    if(!fonts||!attrs||(attrs&7u)||!old)return;
    TextBankPrefix *b=(TextBankPrefix *)(attrs-56u);
    if(b->fonts!=fonts||b->attributes!=(uintptr_t *)attrs||b->languages!=FONT_LANGS||b->stock!=TEXT||
       b->capacity<=b->stock||b->capacity>65536u)return;
    if(b->font_default==FONT_CLASSES)return;
    const FontClassTable *have=(const FontClassTable *)(const void *)old;
    if(have->magic!=FONT_MAGIC) {
        for(uint32_t c=0u;c<FONT_CLASSES;c++)if(!old[c])return;
        const uint8_t *base=old[0];if(((uintptr_t)base&7u)||base[8]!=0x10u)return;   /* English: Latin face */
        FontClassTable *t=FN(0x40d2c408,void *(*)(uint32_t,uint32_t))((uint32_t)sizeof(*t),0u);
        if(!t)return;
        for(uint32_t c=0u;c<FONT_CLASSES;c++)t->classes[c]=old[c];
        const uint64_t *src=(const uint64_t *)(const void *)base;
        for(uint32_t l=0u;l<FONT_LANGS;l++)t->entries[l]=src[font_lang_cjk(l)?1u:l];
        t->classes[FONT_CLASSES]=(const uint8_t *)t->entries;t->magic=FONT_MAGIC;
        __asm__ volatile("dmb ish" ::: "memory");
        *slot=(const uint8_t *const *)t->classes;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    uint8_t old_default=b->font_default;
    for(uint32_t i=b->stock;i<b->capacity;i++)if(fonts[i]==old_default)fonts[i]=(uint8_t)FONT_CLASSES;
    b->font_default=(uint8_t)FONT_CLASSES;
    __asm__ volatile("dmb ish" ::: "memory");
}
static void install(void) {
    /* v49: the v40 class-18 retarget is off. Text ids 3137..3224 are shared with NFP (anamorphic,
     * H.265, Log), whose CJK translations and ASCII option widths need the stock class-0 face;
     * one font class per id cannot serve both. Live avoids the CJK-face 'q' glyph instead. */
    (void)live_font_fix;
    CorePrefix *root=core_root();if(!root||ui())return;
    FxRegistry *g=fx_registry(&root->events[46],1);if(!g||g->live_ui)return;
    /* The playback layer may be inserting its row from another task; it holds
     * the menu lock only briefly and never blocks inside it. */
    uint32_t tries=0u;while(!fx_menu_trylock(g)){if(++tries>200u)return;FN(0x40187b00,int32_t (*)(uint32_t))(5u);}
    FxNativeMenu *core=(FxNativeMenu *)live_old_menu_page(2u,0x7000u);
    int at=fx_core_menu_slot(core,TEXT+2u);
    UiState *u=at<0?0:FN(0x40d2c408,void *(*)(uint32_t,uint32_t))((uint32_t)sizeof(*u),0u);
    if(!u){fx_menu_unlock(g);return;}
    memset(u,0,sizeof(*u));u->command=UINT32_MAX;u->display_status=UINT32_MAX;u->mute_cmd=UINT32_MAX;
    FxNativeMenuProfile p={&core->page,&core->rows[0],(const FxNativeEvent *)(uintptr_t)UINT64_C(0x43bff8e8),
        0x3bbu,0x285u,{0x286u,0x28bu,0x29fu,0x2aeu},0x68cu,0x7000u};
    FxNativeEntry choices[]={{TEXT+14u,0x7fb0u,0u,0u,(uintptr_t)on,0x7000u},
                            {TEXT+15u,0x7fb1u,0u,0u,(uintptr_t)off,0x7000u},
                            {SOUND_TEXT,0x7fb3u,0u,0u,(uintptr_t)sound,0x7000u}};
    if(fx_native_menu_build(&u->child,&p,LIVE_PAGE,LIVE_TEXT,choices,3u,core->callbacks)!=FX_OK ||
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
    if((id!=(int32_t)LIVE_TEXT&&id!=(int32_t)SOUND_TEXT)||!ui()){live_old_menu_text(language,id,text,bytes);return;}
#define LABEL(s) do {static const uint16_t v[]=u##s;*text=v;*bytes=(uint32_t)sizeof(v)-2u;}while(0)
    if(id==(int32_t)SOUND_TEXT) {
        if(live_shutter_mute_get()){LABEL("Shutter sound: Off");}else{LABEL("Shutter sound: On");}
        return;
    }
    uint32_t status=live_status();
    if(live_ui_save_error()){LABEL("Live Photo (card error)");}
    else if(status&LIVE_BUSY){LABEL("Live Photo: Busy");}
    else if(status&LIVE_ERROR){LABEL("Live Photo: Unavailable");}
    else if(status&LIVE_READY){LABEL("Live Photo: Ready");}
    else if(status&LIVE_WARM){LABEL("Live Photo: Short clip");}
    else if(status&LIVE_REQUESTED){LABEL("Live Photo: Preparing");}
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
/* v45: 1 read, 0 absent (stock "not found" 0x80000010), -1 transient (card-slot refusal
 * while the boot card choice is pending, or an IO error): never treated as "no config". */
#define OPEN_NOT_FOUND 0x80000010u
static int read_config(uint32_t slot,uint8_t out[65],uint32_t *bytes) {
    int32_t fd=FN(0x402dd4f8,int32_t (*)(const char *,uint32_t,uint32_t))(path(slot),0u,0u);
    *bytes=0u;if(fd<0)return (uint32_t)fd==OPEN_NOT_FOUND?0:-1;
    uint32_t error=0u;
    while(*bytes<65u) {
        uint32_t got=0u,room=65u-*bytes;
        uint32_t r=FN(0x402dd6c8,uint32_t (*)(int32_t,void *,uint32_t,uint32_t *))(fd,out+*bytes,room,&got);
        if(r||got>room){error=1u;break;}*bytes+=got;if(!got)break;
    }
    uint32_t closed=FN(0x402dd558,uint32_t (*)(int32_t))(fd);
    return !error&&!closed?1:-1;
}
/* v45 (hw 11-12): the first load ran inside the card-slot decision window (24 refused
 * FENESTRA calls in 0.22 s), read nothing, applied Live Off, and a later menu toggle saved
 * that Off as a newer generation. Wait for the card choice, retry transient failures, and
 * fall back to defaults only after 30 s. */
#define CS_STATE ((volatile uint32_t *)(uintptr_t)UINT64_C(0xa1171b000))   /* cardslot: magic 'CSL1', drive */
static uint64_t ui_now_us(void){uint64_t t,f;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(t),"=r"(f));return f?t/f*1000000u+(t%f)*1000000u/f:0;}
static void load_config(UiState *u) {
    uint64_t now=ui_now_us();
    if(!u->load_first_us)u->load_first_us=now?now:1u;
    int late=now-u->load_first_us>30000000u;
    if(!late&&CS_STATE[0]==0x314c5343u&&CS_STATE[1]==0u){u->load_waits++;return;}
    uint64_t best=0u;uint32_t found=0u,wanted=0u,slot=0u,mute=0u,transient=0u;
    for(uint32_t i=0u;i<2u;i++) {
        uint8_t data[65];uint32_t size=0u,r=0u,m=0u;uint64_t generation=0u;
        int rc=read_config(i,data,&size);
        if(rc<0){transient++;continue;}
        if(rc>0&&live_config_valid(data,size,&generation,&r,&m)&&(!found||generation>best)) {
            found=1u;best=generation;wanted=r;slot=i;mute=m;
        }
    }
    if(transient&&!late){u->load_retries++;return;}
    if(transient&&!found&&u->generation){u->loaded=1u;u->load_first_us=0u;return;}   /* reload: keep what is applied */
    u->generation=best;u->slot=slot;u->loaded=1u;u->saved_requested=wanted;u->mute=mute;u->load_first_us=0u;
    if(atomic_load_explicit(&u->command,memory_order_acquire)==UINT32_MAX)live_requested_set(wanted);
    if(atomic_load_explicit(&u->mute_cmd,memory_order_acquire)==UINT32_MAX)live_shutter_mute_set(mute);
}
static int save_config(UiState *u,uint32_t wanted,uint32_t mute) {
    if(u->generation==UINT64_MAX)return 0;
    uint8_t data[64],check[65];uint32_t slot=u->slot^1u;
    live_config_make(data,u->generation+1u,wanted,mute);
    /* Replace only our inactive config slot; the last valid slot is retained.
     * Native0x101 is exclusive creation and cannot rewrite an existing slot. */
    int32_t fd=FN(0x402dd4f8,int32_t (*)(const char *,uint32_t,uint32_t))(path(slot),0x301u,0u);
    if(fd<0)return 0;
    uint32_t written=0u;
    uint32_t error=FN(0x402dd78c,uint32_t (*)(int32_t,const void *,uint32_t,uint32_t *,void *))(fd,data,64u,&written,0);
    uint32_t closed=FN(0x402dd558,uint32_t (*)(int32_t))(fd);
    if(error||closed||written!=64u)return 0;
    uint32_t size=0u,r=0u,m=0u;uint64_t generation=0u;
    if(!read_config(slot,check,&size)||!live_config_valid(check,size,&generation,&r,&m)||generation!=u->generation+1u||r!=wanted||m!=mute)return 0;
    u->slot=slot;u->generation=generation;u->saved_requested=wanted;u->mute=mute;return 1;
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
    UiState *u=(UiState *)((uint8_t *)d - offsetof(UiState,display));
    atomic_fetch_add_explicit(&u->evf_diag.draws,1u,memory_order_relaxed);
    atomic_store_explicit(&u->evf_diag.result,UINT32_MAX,memory_order_relaxed);
    uint64_t ticket=atomic_load_explicit(d->gate,memory_order_seq_cst);
    if(!(ticket&1u))return 1;
    atomic_fetch_add_explicit(&d->readers,1u,memory_order_seq_cst);
    uint32_t result=1;
    if(atomic_load_explicit(d->gate,memory_order_seq_cst)==ticket &&
       FN(0x401a29c4,uint32_t (*)(const FxNativeDisplay *))(d)) {
        result=icon32_paint(d,(const FxNativeDisplayProfile *)(uintptr_t)UINT64_C(0x401dee00),0,&u->icon_backing);
        atomic_store_explicit(&u->evf_diag.result,result,memory_order_relaxed);
        atomic_store_explicit(&u->icon_blocked,result==2u,memory_order_release);
        if(result==2u){d->callbacks++;result=1;}
    }
    atomic_fetch_sub_explicit(&d->readers,1u,memory_order_release);
    return result;
}
/* v46: not ready yet -> same artwork, black outline kept, ink dimmed; the twelve outer arcs
 * light up clockwise from 12 o'clock as the pre-roll fills (ring centre 20,22 in the 110x44 art,
 * arcs at radius 14..17 centred on multiples of 30 degrees). */
#define ICON32_DIM_GAIN 110u
static uint32_t icon32_ring_seg(uint32_t x,uint32_t y) {
    int32_t dx=(int32_t)(2u*x+1u)-40,dy=(int32_t)(2u*y+1u)-44;
    if(x>=40u||dx*dx+dy*dy<26*26)return 12u;
    uint32_t ax=(uint32_t)(dx<0?-dx:dx),ay=(uint32_t)(dy<0?-dy:dy);
    uint32_t j=ay*1000u<268u*ax?0u:ay<ax?1u:ay*1000u<3732u*ax?2u:3u;
    return dx>=0&&dy>=0?j:dx<0&&dy>=0?6u-j:dx<0?6u+j:(12u-j)%12u;
}
static void icon32_state_rgba(const Icon32Asset *a,uint32_t i,uint32_t phase,int busy,uint32_t progress,uint8_t out[4]) {
    if(busy||progress>=12u){icon32_shoot_rgba(a,i,phase,busy,out);return;}
    uint8_t pair=icon32_shoot_pixel(a,i),alpha=pair&15u;
    uint32_t k=icon32_ring_seg(i%a->w,i/a->w);
    uint32_t gain=k<12u&&(k+3u)%12u<progress?255u:ICON32_DIM_GAIN;
    out[0]=out[1]=out[2]=alpha?(uint8_t)(((pair>>4)*gain+alpha/2u)/alpha):0u;
    out[3]=(uint8_t)(alpha*17u);
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
        uint32_t progress=status&(LIVE_READY|LIVE_BUSY)?12u:(status>>LIVE_PROGRESS_SHIFT)&15u;
        for(uint32_t i=0u;i<w*h;i++) {
            icon32_state_rgba(&asset,i,phase,(status&LIVE_BUSY)!=0,progress,p+4u*i);
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
/* v44 (hw 11-12): leaving playback for the shooting view, the stock UI disables the global
 * graphics target that attach enabled (target_was_enabled 0), while the surface stays visible
 * and our object stays listed: presents keep "succeeding" but nothing reaches the LCD.
 * Query the target through the core profile and re-attach (which enables it again) when it was switched off. */
/* v47: parenthesised. v44-v46 expanded `*(T*)(uintptr_t)0x401dee00+192`, i.e. (first profile word)+192,
 * so the check never matched and the target was never re-attached on hw. */
#define ND_PROFILE_TARGET_ENABLED (UINT64_C(0x401dee00)+192u)
static int overlay_target_off(const UiState *u) {
    if(!u->display.attached||!u->display.global_overlay)return 0;
    /* v52: the pinned profile adapts GUI (group,output) to native (output,group). */
    uintptr_t enabled=*(const volatile uintptr_t *)(uintptr_t)ND_PROFILE_TARGET_ENABLED;
    return !(FN(enabled,uint32_t (*)(uint32_t,uint32_t,uint32_t))
        (u->display.target[0],u->display.target[1],u->display.target[2])&1u);
}
static void indicator(UiState *u,uint32_t status) {
    uint32_t photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0u;
    if(!photo||!(status&LIVE_REQUESTED)||atomic_load(&u->shutdown)){hide(u);return;}
    if(u->display.attached&&!fx_native_display_is_current(&u->display)){u->not_current++;icon32_unlink_stale(u);return;}
    if(overlay_target_off(u)){u->target_lost++;icon32_unlink_stale(u);return;}
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
/* The shared core profile points here only in the diagnostic candidate.
 * Count the actual native submit call, preserving all arguments and return. */
__attribute__((section(".live_extra"),noinline))
uint64_t live_ui_evf_submit(void *progress,uint32_t groups,uint32_t outputs) {
    UiState *u=ui();if(u)atomic_fetch_add_explicit(&u->evf_diag.submits,1u,memory_order_relaxed);
    return FN(0x403efed8,uint64_t (*)(void *,uint32_t,uint32_t))(progress,groups,outputs);
}
/* Sole UI worker; four bounded reads under the same GUI exclusion as drawing.
 * Fixed root is static native RAM. Foreign pointers are never followed. */
__attribute__((section(".live_extra"),noinline))
static void live_ui_evf_sample(UiState *u) {
    struct evf_diag *q=&u->evf_diag;uint32_t n=q->used;
    if(n>=EVF_DIAG_SAMPLES)return;
    uint32_t output=*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03229eb0)?4u:1u;
    uint64_t tick,hz;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(tick),"=r"(hz));
    if(!hz || (output==4u ? (n>=3u || tick<q->next || !(live_status()&LIVE_REQUESTED)) : !n))return;
    icon32_lock();
    /* A switch during mutex acquisition invalidates this observation. */
    if(output!=(*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03229eb0)?4u:1u)){icon32_unlock();return;}
    struct evf_diag_sample *sample=&q->samples[n];uint32_t *w=sample->words;
    FxNativeDisplay *d=&u->display;sample->timestamp_us=tick/hz*1000000u+(tick%hz)*1000000u/hz;
    w[0]=output;w[1]=live_status();w[2]=(uint32_t)atomic_load(&d->attached)|((uint32_t)d->global_overlay<<1)|
        (atomic_load(&u->shutdown)<<2)|(u->loaded<<3)|(atomic_load(&u->icon_blocked)<<4)|((atomic_load(&u->gate)&1u)<<5);
    uintptr_t view=*(volatile uintptr_t *)(uintptr_t)UINT64_C(0xa032280c8);
    w[3]=(uint32_t)view;w[4]=(uint32_t)(view>>32);w[5]=UINT32_MAX;
    if(view>=UINT64_C(0xa00000000)&&view<=UINT64_C(0xa40000000)-0x110u&&!(view&7u))
        w[5]=(*(volatile uint32_t *)(view+0x54u)&65535u)|(*(volatile uint32_t *)(view+0xd0u)<<16);
    w[6]=(uint32_t)(uintptr_t)d->surface;w[7]=(uint32_t)((uintptr_t)d->surface>>32);
    const volatile uint32_t *root=(const volatile uint32_t *)(uintptr_t)UINT64_C(0xa0322a340);
    w[8]=root[0];for(uint32_t i=0;i<3;i++)w[9+i]=root[0x108u/4u+i];
    if(w[9]<=2u&&(w[10]==1u||w[10]==4u)&&w[11]<=4u) {
        const volatile uint8_t *p=FN(0x4033d8cc,const volatile uint8_t *(*)(uint32_t,uint32_t,uint32_t))(w[9],w[10],w[11]);
        if(p){uintptr_t b=*(const volatile uintptr_t *)(p+0x28u);w[12]=(uint32_t)b;w[13]=(uint32_t)(b>>32);
            w[14]=*(const volatile uint32_t *)(p+8u);w[2]|=(uint32_t)p[0x4c]<<8|(*(const volatile uint32_t *)(p+0x3cu)&255u)<<16;}
    }
    w[15]=atomic_load_explicit(&q->draws,memory_order_relaxed);w[16]=atomic_load_explicit(&q->result,memory_order_relaxed);
    w[17]=atomic_load_explicit(&q->submits,memory_order_relaxed);w[18]=d->callbacks;w[19]=d->error;w[20]=u->display_status;
    w[21]=u->presents;w[22]=u->present_fail;w[23]=(u->not_current&65535u)|(u->target_lost<<16);
    icon32_unlock();q->next=tick+hz;q->used=output==1u?EVF_DIAG_SAMPLES:n+1u;
    atomic_store_explicit(&q->published,(n+1u)|(output==1u?0x80000000u:0u),memory_order_release);
}
/* Called by log_step only after its existing write-rate gate. No GUI lock.
 * Immutable published records become ordinary LLG2 crumbs immediately before
 * the normal file write. The original log format and writer are unchanged. */
__attribute__((section(".live_extra"),noinline))
void live_ui_evf_flush(struct live_runtime *r) {
    UiState *u=ui();if(!u)return;struct evf_diag *q=&u->evf_diag;
    uint32_t published=atomic_load_explicit(&q->published,memory_order_acquire);
    if(q->drained||!(published&0x80000000u)||
       *(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03229eb0)||
       __atomic_load_n(&r->stream.job,__ATOMIC_ACQUIRE)||__atomic_load_n(&r->writer_active,__ATOMIC_ACQUIRE)||
       (__atomic_load_n(&r->capture.phase,__ATOMIC_ACQUIRE)>=LA_POST && __atomic_load_n(&r->capture.phase,__ATOMIC_ACQUIRE)<=LA_ENCODING)||
       __atomic_load_n(&r->capture.press_mailbox,__ATOMIC_ACQUIRE)||
       (__atomic_load_n(&LIVE_TWIN->state,__ATOMIC_ACQUIRE)==2u))return;
    for(uint32_t n=0;n<(published&7u);n++)for(uint32_t k=0;k<8u;k++) {
        const uint32_t *w=q->samples[n].words+3u*k;
        uint64_t t=q->samples[n].timestamp_us,b=(uint64_t)w[1]|((uint64_t)w[2]<<32);
        live_log(&r->stream,t,61u+k,w[0],b);
        uint32_t i=__atomic_fetch_add(&r->crumb_head,1u,__ATOMIC_RELAXED)%32u;
        r->crumbs[i]=(struct live_ev){t,61u+k,w[0],b};
    }
    q->drained=1u;
}

void live_ui_poll(void) {
    UiState *u=ui();if(!u)return;
    live_ui_evf_sample(u);
    if(atomic_load(&u->shutdown)){hide(u);return;}
    if(atomic_exchange_explicit(&u->reload,0u,memory_order_acq_rel))u->loaded=0u;
    if(!u->loaded)load_config(u);
    if(!u->loaded){indicator(u,live_status());return;}   /* v45: menu commands wait for the saved state */
    uint32_t wanted=atomic_exchange_explicit(&u->command,UINT32_MAX,memory_order_acq_rel);
    uint32_t mute=atomic_exchange_explicit(&u->mute_cmd,UINT32_MAX,memory_order_acq_rel);
    if(wanted<=1u||mute<=1u)
        atomic_store(&u->save_error,save_config(u,wanted<=1u?wanted:u->saved_requested,mute<=1u?mute:u->mute)?0u:1u);
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
