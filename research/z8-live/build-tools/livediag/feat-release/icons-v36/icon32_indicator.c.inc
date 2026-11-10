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
    u->display_status=UINT32_MAX;u->icon_retry=0u;u->icon_fallback_until=0u;
}
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
        u->icon_retry=0u;u->presents++;if(refresh)u->refreshes++;
    } else {
        u->present_fail++;u->icon_retry=1u;
        if(rt)live_log(&rt->stream,t,EV_ICON,status,u->display.error);
        /* Retry on the existing attachment. */
    }
}
