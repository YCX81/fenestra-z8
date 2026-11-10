/* Native Fenestra REC/Off menu for the exact fix-20261104 core.
 * Labels use unused IDs inside the existing 26-entry native font extension.
 * The original seven service rows and their callbacks remain core-owned. */
#include "settings.h"
#include "runtime.h"
#include "fenestra_memory.h"
#include "fx_registry.h"
extern FxRegistry *pb_registry(void);
#include <stdatomic.h>
#define TEXT_BASE 3154u
#define LABEL_KEY (TEXT_BASE+6u)
#define LABEL_REC (TEXT_BASE+7u)
#define LABEL_TITLE (TEXT_BASE+8u)
#define LABEL_OFF (TEXT_BASE+15u)
#define FENESTRA UINT32_C(0x7000)
static uint32_t noop(void *arg) {(void)arg;return 1u;}
static uint32_t choose_rec(void *arg) {(void)arg;pb_set_key(9u);return 1u;}
static uint32_t choose_off(void *arg) {(void)arg;pb_set_key(0u);return 1u;}
PbSettings *pb_settings_state(void) {
    PbRuntime *r=pb_runtime_get();
    return r?(PbSettings *)__atomic_load_n(&r->settings,__ATOMIC_ACQUIRE):0;
}
void pb_settings_poll(PbRuntime *r) {
#ifdef PB_SETTINGS_DISABLED
    /* Combined with plan A's Live Photo row: both insert at core row 6 and
     * use core handlers[7], so only one may own the Fenestra page. Without
     * the state object the page/text hooks below pass straight through and
     * the playback key stays at its default (REC). */
    (void)r;return;
#endif
    if(!r)return;
    PbSettings *s=(PbSettings *)__atomic_load_n(&r->settings,__ATOMIC_ACQUIRE);
    if(!s) {
        s=((void *(*)(uint32_t,uint32_t))(uintptr_t)UINT64_C(0x40d2c408))((uint32_t)sizeof(*s),0u);
        if(!s)return;
        memset(s,0,sizeof(*s));
        __atomic_store_n(&r->settings,s,__ATOMIC_RELEASE);
    }
    if(s->installed || s->error)return;
    /* Metadata publication is under the same native GUI exclusion as drawing.
     * No card I/O and no callback or worker wait occur inside this section. */
    ((void (*)(void *))(uintptr_t)UINT64_C(0x40d2bfb4))((void *)(uintptr_t)UINT64_C(0xa032bbd68));
    (void)pb_settings_install(s);
    ((void (*)(void *))(uintptr_t)UINT64_C(0x40d2bf60))((void *)(uintptr_t)UINT64_C(0xa032bbd68));
}
int pb_settings_install(PbSettings *s) {
    if(!s)return 0;
    if(s->installed)return 1;
    /* Insert just before Back under the shared menu lock, so plan A's Live
     * Photo row and ours can install in either order. Busy: retry next poll. */
    FxRegistry *g=pb_registry();if(!g||!fx_menu_trylock(g))return 0;
    FxNativeMenu *core=(FxNativeMenu *)pb_settings_old_menu_page(2u,FENESTRA);
    if(!core){fx_menu_unlock(g);return 0;} /* Native core has not installed yet; caller may retry. */
    int at=fx_core_menu_slot(core,TEXT_BASE+2u);
    if(at<0){fx_menu_unlock(g);s->error=1u;return 0;}
    FxNativeMenuProfile profile={&core->page,&core->rows[0],
        (const FxNativeEvent *)(uintptr_t)UINT64_C(0x43bff8e8),
        0x3bbu,0x285u,{0x286u,0x28bu,0x29fu,0x2aeu},0x68cu,FENESTRA};
    FxNativeEntry choices[]={
        {LABEL_REC,0x7fb5u,0u,0u,(uintptr_t)choose_rec,FENESTRA},
        {LABEL_OFF,0x7fb6u,0u,0u,(uintptr_t)choose_off,FENESTRA}
    };
    FxResult result=fx_native_menu_build(&s->choices,&profile,PB_SETTINGS_PAGE,LABEL_TITLE,
                                        choices,2u,core->callbacks);
    if(result!=FX_OK){fx_menu_unlock(g);s->error=2u;return 0;}
    /* handlers[at+1] belongs to the row slot being inserted; do not inherit an
     * already-rewritten child event array as a navigation template. */
    result=fx_native_menu_link(core->handlers[at+1],profile.navigation,profile.terminal,
                              profile.select_events,(uintptr_t)noop,PB_SETTINGS_PAGE);
    if(result!=FX_OK){fx_menu_unlock(g);s->error=3u;return 0;}
    s->core=core;s->installed=1u;
    fx_core_menu_commit(core,(unsigned)at,LABEL_KEY,PB_SETTINGS_ROW);
    fx_menu_unlock(g);
    return 1;
}
FxNativePage *pb_settings_menu_page(uint32_t group,uint32_t id) {
    PbSettings *s=pb_settings_state();
    id&=65535u;
    if(group==2u && (id==FENESTRA || id==PB_SETTINGS_PAGE) && s) {
        if(!s->installed)(void)pb_settings_install(s);
        if(id==PB_SETTINGS_PAGE) {
            if(!s->installed)return 0;
            s->choices.page.selected=(uint16_t)(pb_get_key()?0u:1u);
            return &s->choices.page;
        }
    }
    return pb_settings_old_menu_page(group,id);
}
void pb_settings_menu_text(int32_t language,int32_t id,const uint16_t **text,uint32_t *bytes) {
    PbSettings *s=pb_settings_state();
    if(!s || !s->installed || !text || !bytes) {
        pb_settings_old_menu_text(language,id,text,bytes);return;
    }
#define LABEL(value) do {static const uint16_t v[]=u##value;*text=v;*bytes=(uint32_t)sizeof(v)-2u;return;}while(0)
    if(id==(int32_t)LABEL_TITLE){LABEL("Live playback key");}
    if(id==(int32_t)LABEL_REC){LABEL("REC");}
    if(id==(int32_t)LABEL_KEY) {
        if(pb_get_key()){LABEL("Live playback key: REC");}
        else {LABEL("Live playback key: Off");}
    }
#undef LABEL
    pb_settings_old_menu_text(language,id,text,bytes);
}
