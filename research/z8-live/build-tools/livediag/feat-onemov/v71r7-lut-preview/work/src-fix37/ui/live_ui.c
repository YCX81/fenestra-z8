/* Overlay for the pinned fix-20261104 core. No writable statics/RX state. */
#include "live_ui.h"
#include "icon32_shoot.h"
#include "icon32_layout.h"
#include "fenestra_memory.h"
#include "fenestra_native_menu.h"
#include "fenestra_native_display.h"
#include "fx_registry.h"
#include <stdatomic.h>
#include <stddef.h>
#include "runtime.h"
#include "evf_diag.h"
#include "filmsim_runtime.h"
#define FN(a,t) ((t)(uintptr_t)(a))
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
    _Atomic(void *) filmsim_context; /* filmsim-wlm: appended; earlier offsets unchanged */
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
_Atomic(void *) *live_ui_filmsim_context_slot(void) {UiState *u=ui();return u?&u->filmsim_context:0;}
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
int livephoto_is_module_loaded(void) {
    static const char k_lp_id[16]="livephoto";
    return FN(0x401ccde4,uint32_t (*)(const char *))(k_lp_id)!=0u;
}
void live_shutter_mute_set(uint32_t mute) {UiState *u=ui();if(u)atomic_store(&u->mute_live,mute?1u:0u);}
uint32_t live_shutter_mute_get(void) {UiState *u=ui();return u?atomic_load(&u->mute_live):0u;}
/* v39/v71.7-r4: stock shutter-sound task, at its mute check (hooks.S live_shutter_entry).
 * Mute only while livephoto.wlm is loaded and Live is requested in photo mode.
 * Lock-free, no IO. */
uint32_t live_shutter_gate(uint32_t stock) {
    live_shutter_diag(3u,stock);
    UiState *u=ui();if(!u)return stock;
    atomic_fetch_add(&u->shutter_seen,1u);
    if(stock||!livephoto_is_module_loaded())return stock;
    int photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0;
    if(!photo||!atomic_load(&u->mute_live)||atomic_load(&u->shutdown)||!(live_status()&LIVE_REQUESTED))return 0u;
    atomic_fetch_add(&u->shutter_suppressed,1u);return 1u;
}
/* V71.7-r4: Fenestra home page 0x7000 stays pure 7 stock core rows (Live Photo
 * UI & config persistence moved to livephoto.wlm EL0). Still allocates UiState,
 * publishes g->live_ui (for filmsim_context/engine/display/evf_diag), and boots
 * the runtime worker/mux_worker (needed for FilmSim UI poll & USB flag_step). */
static void install(void) {
    CorePrefix *root=core_root();if(!root||ui())return;
    FxRegistry *g=fx_registry(&root->events[46],1);if(!g||g->live_ui)return;
    UiState *u=FN(0x40d2c408,void *(*)(uint32_t,uint32_t))((uint32_t)sizeof(*u),0u);
    if(!u)return;
    memset(u,0,sizeof(*u));u->command=UINT32_MAX;u->display_status=UINT32_MAX;u->mute_cmd=UINT32_MAX;
    if(!fx_registry_claim(&g->live_ui,(uintptr_t)u)) {
        FN(0x40d2c448,void (*)(void *))(u);return;
    }
    u->scene=(FxDemoScene){720u,480u,16u,0u,u->commands,0,0u,0u};
    u->display.gate=&u->gate;u->display.exact_colors=1u;
    /* Publish last: ui() only returns objects carrying the magic. */
    __atomic_store_n(&u->magic,UI_MAGIC,__ATOMIC_RELEASE);
    live_runtime_boot();
}
extern void v71r7_photo_menu_boot_once(void);
extern void v71r7_photo_menu_on_card_change(void);
extern int  v71r7_photo_menu_lookup_text(int32_t language, uint32_t text_id,
                                         const uint16_t **text_out, uint32_t *bytes_out);
extern void photo_worker_shutdown(void);

FxNativePage *live_ui_menu_page(uint32_t group,uint32_t id) {
    return live_old_menu_page(group,id);
}
void live_ui_menu_text(int32_t language,int32_t id,const uint16_t **text,uint32_t *bytes) {
    if(id>=0x1F40&&id<=0x1F4A&&v71r7_photo_menu_lookup_text(language,(uint32_t)id,text,bytes))return;
    live_old_menu_text(language,id,text,bytes);
}
uint64_t live_ui_system_dispatch(void *message,uint32_t sync) {
    uint32_t event=message?*(const uint32_t *)((const uint8_t *)message+32u):UINT32_MAX;
    UiState *u=ui();
    if(u&&(event==33u||event==34u)){atomic_store(&u->usb,event==33u);atomic_fetch_add(&u->usb_events,1u);}
    if(u&&(event==7u||event==8u||event==14u||event==15u||event==16u||event==24u||event==33u||event==34u))
        live_runtime_ui_event(event|atomic_load(&u->usb)<<8);
    /* v71.5 A1: ev=7 (BSShutdown) fires for cancellable standby/auto-off even
     * without USB (followed by ev=8 BSShutdownCancel on wake). Always flush
     * LIVELOG and park BOOT-Z8.FLAG now (so a subsequent power-cut leaves no
     * stale flag), but never forward ev=7 into live_old_system_dispatch (which
     * runs irreversible stop_all() killing all .wlm plugins) and never latch
     * u->shutdown=1 here. */
    if(u&&event==7u) {
        live_runtime_flag_park(1);
        atomic_fetch_add(&u->standby_kept,1u);
        return ((uint64_t (*)(void *,uint32_t))(uintptr_t)UINT64_C(0x401d6000))(message,sync);
    }
    /* v71.5 A1: ev=8 (BSShutdownCancel) / ev=15 (BSSendPowerDownCancelDone):
     * immediately restore parked BOOT-Z8.FLAG; plugins/Live/FilmSim stay live. */
    if(u&&(event==8u||event==15u)) {
        live_runtime_flag_park(0);
    }
    /* v71.5 A1: ev=14 (BSSendPowerDownDone): restore C64 proxy PC into stock
     * settings while tasks are alive, without latching irreversible shutdown
     * (ev=14 can still cancel via ev=8/12/15 before NonReleasePhase). */
    if(u&&event==14u) {
        FN(0x401a6908,void (*)(void))();
    }
    /* v71.6/v71.7 hotfix: ev=16 (eSyscCommandId_BSWaitEnableSuspend) is sent by
     * NonReleasePhase (0x40d6a6a0 -> 0x40d60d10 -> 0x40d61180) at the end of
     * normal cold boot (~1.75s on real Z8) to announce suspend is now enabled.
     * It is NOT a shutdown event; never shut down photo_worker, latch u->shutdown=1,
     * or stop FilmSim/Layer 0! */
    if(event==24u) {
        if(u&&live_ui_engine_get()) {
            atomic_store(&u->command,UINT32_MAX);
            live_runtime_event(24u); /* cascades once to filmsim_runtime_on_card_change()->v71r7_photo_menu_on_card_change() */
        } else {
            v71r7_photo_menu_on_card_change();
        }
    }
    uint64_t result=live_old_system_dispatch(message,sync);
    if(u&&event==24u)atomic_store(&u->reload,1u);
    if(event==19u){install();v71r7_photo_menu_boot_once();}
    return result;
}
#define CS_STATE ((volatile uint32_t *)(uintptr_t)UINT64_C(0xa1171b000))   /* cardslot: magic 'CSL1', drive */
extern int live_encoder_is_parked(const live_encoder *e);

/* Cross-bank PbRuntime layout (Stage 0 pb-fix37/runtime.c asserts exact offsets). */
#define PB_RT_MAGIC         UINT32_C(0x50425549)
#define PB_RT_MIN_BYTES     9672u
#define PB_RT_OFF_KEY       8u
#define PB_RT_OFF_KEY_EPOCH 20u
#define PB_RT_OFF_GATE      32u
#define PB_RT_OFF_ERROR     44u
#define PB_RT_OFF_FRAMES    72u
#define PB_RT_OFF_CONSUMED  8224u
#define PB_RT_OFF_NB_STARTS 9368u
#define PB_RT_OFF_NB_FAIL   9404u

static void lp_copy(void *dst,const void *src,size_t n) {
    uint8_t *d=(uint8_t *)dst;const uint8_t *s=(const uint8_t *)src;
    while(n--)*d++=*s++;
}
static uint16_t lp_sat16(uint32_t v) { return v>65535u?65535u:(uint16_t)v; }
static uint8_t lp_active_drive(void) {
    return (CS_STATE[0]==0x314c5343u&&CS_STATE[1]==2u)?(uint8_t)'B':(uint8_t)'A';
}
static uint8_t *lp_pb_raw(FxRegistry *g) {
    if(!g)return 0;
    uintptr_t p=__atomic_load_n(&g->playback,__ATOMIC_ACQUIRE);
    if(!fx_registry_ram(p,PB_RT_MIN_BYTES))return 0;
    uint8_t *b=(uint8_t *)p;
    if(*(const uint32_t *)(const void *)(b+0u)!=PB_RT_MAGIC||
       *(const uint32_t *)(const void *)(b+4u)<PB_RT_MIN_BYTES)return 0;
    return b;
}
static uint8_t lp_get_pb_key(FxRegistry *g,uint8_t *pb) {
    if(pb) {
        uint32_t k=__atomic_load_n((uint32_t *)(void *)(pb+PB_RT_OFF_KEY),__ATOMIC_ACQUIRE);
        return k==9u?9u:0u;
    }
    if(g) {
        uintptr_t latch=__atomic_load_n(&g->spare[1],__ATOMIC_ACQUIRE);
        if(latch==1u)return 0u;
        if(latch==2u)return 9u;
    }
    return 9u;
}
static void lp_set_pb_key(FxRegistry *g,uint8_t key) {
    uint32_t k=key==9u?9u:0u;
    if(g)__atomic_store_n(&g->spare[1],k?2u:1u,__ATOMIC_RELEASE);
    uint8_t *pb=lp_pb_raw(g);
    if(pb) {
        __atomic_store_n((uint32_t *)(void *)(pb+PB_RT_OFF_KEY),k,__ATOMIC_RELEASE);
        __atomic_fetch_add((uint32_t *)(void *)(pb+PB_RT_OFF_KEY_EPOCH),1u,__ATOMIC_RELAXED);
        uint64_t *gp=(uint64_t *)(void *)(pb+PB_RT_OFF_GATE);
        uint64_t old=__atomic_load_n(gp,__ATOMIC_ACQUIRE),next;
        do { next=(old&~UINT64_C(1))+2u; }
        while(!__atomic_compare_exchange_n(gp,&old,next,1,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED));
    }
}
static void lp_fill_state(LivePhotoIpcState *out) {
    memset(out,0,sizeof(*out));
    out->magic=LP_IPC_MAGIC;
    out->active_drive=lp_active_drive();
    CorePrefix *s=core_root();
    FxRegistry *g=s?fx_registry(&s->events[46],0):0;
    uint8_t *pb=lp_pb_raw(g);
    int lp_loaded=livephoto_is_module_loaded();
    out->pb_key=lp_get_pb_key(g,pb);
    out->mute=(uint8_t)(live_shutter_mute_get()?1u:0u);
    uint8_t w5f=0u;
    if(lp_loaded)w5f|=LP_W5_FLAG_LP_LOADED;
    UiState *u=ui();
    if(u&&atomic_load(&u->reload))w5f|=LP_W5_FLAG_RELOAD;
    if(pb) {
        w5f|=LP_W5_FLAG_PB_READY;
        out->pb_consumed_rec=*(const uint32_t *)(const void *)(pb+PB_RT_OFF_CONSUMED);
        out->pb_frames=*(const uint32_t *)(const void *)(pb+PB_RT_OFF_NB_STARTS)+
                       *(const uint32_t *)(const void *)(pb+PB_RT_OFF_FRAMES);
        uint32_t perr=*(const uint32_t *)(const void *)(pb+PB_RT_OFF_ERROR);
        out->pb_err=perr?perr:*(const uint32_t *)(const void *)(pb+PB_RT_OFF_NB_FAIL);
    }
    struct live_runtime *r=(struct live_runtime *)live_ui_engine_get();
    if(r && r->magic==LIVE_RUNTIME_MAGIC && r->size==sizeof(*r)) {
        uint32_t st=live_status();
        out->requested=(uint8_t)((st&LIVE_REQUESTED)?1u:0u);
        if(live_ui_save_error())     out->ui_phase=LP_UI_CARD_ERR;
        else if(st&LIVE_BUSY)        out->ui_phase=LP_UI_BUSY;
        else if(st&LIVE_ERROR)       out->ui_phase=LP_UI_UNAVAILABLE;
        else if(st&LIVE_READY)       out->ui_phase=LP_UI_READY;
        else if(st&LIVE_WARM)        out->ui_phase=LP_UI_SHORT_CLIP;
        else if(st&LIVE_REQUESTED)   out->ui_phase=LP_UI_PREPARING;
        else                         out->ui_phase=LP_UI_OFF;
        out->audio_phase=(uint8_t)(__atomic_load_n(&r->audio_phase,__ATOMIC_ACQUIRE)&0xffu);
        out->w5_parked=(uint8_t)(live_encoder_is_parked(&r->stream.encoder)?1u:0u);
        if(live_encoder_has_pool(&r->stream.encoder))w5f|=LP_W5_FLAG_HAS_POOL;
        if(r->w5_mirror.close_failed)w5f|=LP_W5_FLAG_CLOSE_FAILED;
        out->tx_count=r->transaction_count;
        out->tx_err=r->transaction_error;
        out->encoded=(uint32_t)r->stream.encoded;
        out->dropped=(uint32_t)(r->stream.copy_dropped+r->stream.backlog_dropped);
        out->audio_tracks=r->audio_tracks;
        out->flag_parks=lp_sat16(r->flag_parks);
        out->flag_restores=lp_sat16(r->flag_restores);
        out->log_saves=r->log_saves;
        out->log_err=r->log_err;
    } else {
        out->ui_phase=LP_UI_UNAVAILABLE;
    }
    const struct live_twin *tw=LIVE_TWIN;
    if(tw->magic==LIVE_TWIN_MAGIC) {
        out->twin_copies=lp_sat16(tw->copies);
        out->twin_err=lp_sat16(tw->errors);
    }
    out->w5_flags=w5f;
}
/* V71.7-r4 Live Photo slots.state EL1 IPC handler (ops 49..51, magic 'LPI1').
 * Dispatched from filmsim_slots_svc_dispatch; lives in .text (live_primary). */
int live_ipc_service_call(uint32_t raw_op,const void *req,uint32_t req_bytes,
                          void *resp,uint32_t *resp_bytes) {
    if(!resp_bytes)return -2;
    if(raw_op==LP_OP_GET_STATE) {
        if(req_bytes!=0u||!resp||*resp_bytes<sizeof(LivePhotoIpcState))return -2;
        LivePhotoIpcState st;
        lp_fill_state(&st);
        lp_copy(resp,&st,sizeof(st));
        *resp_bytes=(uint32_t)sizeof(st);
        return 0;
    }
    if(raw_op==LP_OP_SET_CFG) {
        if(!req||req_bytes!=sizeof(LivePhotoIpcSetCfg))return -2;
        LivePhotoIpcSetCfg cfg;
        lp_copy(&cfg,req,sizeof(cfg));
        if(cfg.magic!=LP_IPC_MAGIC)return -2;
        uint8_t mask=cfg.apply_mask?cfg.apply_mask:LP_SET_MASK_ALL;
        if((mask&LP_SET_MASK_REQUESTED)&&cfg.requested>1u)return -2;
        if((mask&LP_SET_MASK_MUTE)&&cfg.mute>1u)return -2;
        if((mask&LP_SET_MASK_PB_KEY)&&cfg.pb_key!=0u&&cfg.pb_key!=9u)return -2;
        if(!livephoto_is_module_loaded())return -1; /* FX_STATE: retry once phase==2 */
        UiState *u=ui();
        if(!u){install();u=ui();}
        if(!u||atomic_load(&u->shutdown))return -1;
        CorePrefix *s=core_root();
        FxRegistry *g=s?fx_registry(&s->events[46],0):0;
        if(mask&LP_SET_MASK_REQUESTED)live_requested_set((uint32_t)cfg.requested);
        if(mask&LP_SET_MASK_MUTE)live_shutter_mute_set((uint32_t)cfg.mute);
        if(mask&LP_SET_MASK_PB_KEY)lp_set_pb_key(g,cfg.pb_key);
        atomic_store(&u->reload,0u);
        u->loaded=1u;
        if(resp&&*resp_bytes>=sizeof(LivePhotoIpcState)) {
            LivePhotoIpcState st;
            lp_fill_state(&st);
            lp_copy(resp,&st,sizeof(st));
            *resp_bytes=(uint32_t)sizeof(st);
        } else {
            *resp_bytes=0u;
        }
        return 0;
    }
    if(raw_op==LP_OP_FLUSH_LOG) {
        if(req_bytes!=0u)return -2;
        struct live_runtime *r=(struct live_runtime *)live_ui_engine_get();
        if(!r||r->magic!=LIVE_RUNTIME_MAGIC||r->size!=sizeof(*r))return -1;
        __atomic_store_n(&r->log_force_req,1u,__ATOMIC_RELEASE);
        *resp_bytes=0u;
        return 0;
    }
    return -3; /* FX_CAPABILITY */
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
        uint32_t progress=status&(LIVE_READY|LIVE_WARM|LIVE_BUSY)?12u:(status>>LIVE_PROGRESS_SHIFT)&15u;
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
__attribute__((noinline))
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
__attribute__((noinline))
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
    if(atomic_load(&u->shutdown)){filmsim_runtime_on_stop();hide(u);return;}
    /* FilmSim MUST ALWAYS be polled unconditionally, independent of livephoto.wlm! */
    filmsim_runtime_ui_poll();
    if(!livephoto_is_module_loaded()) {
        if(live_shutter_mute_get())live_shutter_mute_set(0u);
        if(live_status()&LIVE_REQUESTED)live_requested_set(0u);
        if(u->display.attached||u->display_status!=UINT32_MAX)hide(u);
        return;
    }
    live_ui_evf_sample(u);
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
