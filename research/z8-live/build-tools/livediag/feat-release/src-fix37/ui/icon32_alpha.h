#ifndef ICON32_ALPHA_H
#define ICON32_ALPHA_H
#include "fenestra_native_display.h"

/* This code is invoked inside the native owner draw on reserved foreground
 * root 4. KEEP's first RECT supplies bounds to the pinned scene validator.
 * The badge renderer never paints that command or a target-selection pixel. */
#ifndef ICON32_NATIVE_ADDR
#define ICON32_NATIVE_ADDR(a) ((uintptr_t)UINT64_C(a))
#endif
#ifndef ICON32_SELECT
/* Called only while the GUI and graphics locks are held. 4032586c forwards
 * the root tuple in this order. 4033e748 tests bit 0 of its fourth argument
 * to skip its own graphics acquire/release. The normal wrapper always passes
 * zero. A missing backing is a *successful no-op* to that native function. */
static uintptr_t icon32_select_locked(const FxNativeDisplay *d) {
    const uint32_t *t=d->target;
    const volatile uint32_t *root=(const volatile uint32_t *)((const uint8_t *)d->surface+0x108u);
    if(t[0]>2u || t[1]!=1u || t[2]>4u || t[0]!=root[0] || t[1]!=root[1] || t[2]!=root[2])return 0;
    const volatile uint8_t *desc=((const uint8_t *(*)(uint32_t,uint32_t,uint32_t))
        ICON32_NATIVE_ADDR(0x4033d8cc))(t[0],t[1],t[2]);
    if(!desc)return 0;
    uintptr_t backing=*(const volatile uintptr_t *)(desc+0x28u);
    if(!backing ||
       *(const volatile uint16_t *)(desc+8u)!=d->width ||
       *(const volatile uint16_t *)(desc+10u)!=d->height ||
       *(const volatile uint32_t *)(desc+0x3cu)>6u)return 0;
    /* The only registered notification writes the GUI tuple cache. Reject
     * a different callback rather than executing unknown code under VG lock. */
    uintptr_t callback=*(volatile uintptr_t *)ICON32_NATIVE_ADDR(0xa02f35f50);
    if(callback && callback!=UINT64_C(0x40325980))return 0;
    uint32_t ok=((uint32_t (*)(uint32_t,uint32_t,uint32_t,uint32_t))
        ICON32_NATIVE_ADDR(0x4033e748))(t[0],t[1],t[2],1u);
    return (ok&1u)?backing:0;
}
#define ICON32_SELECT(d) icon32_select_locked(d)
#endif
#define ICON32_VG(p,member,type) ((type)(uintptr_t)((p)->member))
/* Under GUI exclusion, permit retiring an old viewport only on the same
 * initialized native root and view. The graphics-lock check separately
 * requires the identical backing allocation and dimensions for an erase. */
static inline int icon32_owner(const FxNativeDisplay *d,const FxNativeDisplayProfile *p) {
    if(!d->global_overlay || !(uintptr_t)d->surface || !d->application ||
       *(volatile uintptr_t *)p->current_view_pointer!=d->application)return 0;
    void *view=(void *)d->application;
    const uint32_t *tuple=(const uint32_t *)((const uint8_t *)d->surface+0x108u);
    return ICON32_VG(p,native_view_id,uint32_t (*)(void *))(view)==p->shooting_view_id &&
        ICON32_VG(p,native_view_state,uint32_t (*)(void *))(view)==1u &&
        ICON32_VG(p,native_global_surface,void *(*)(void *,uint32_t))(0,p->global_surface_index)==d->surface &&
        tuple[0]==d->target[0] && tuple[1]==d->target[1] && tuple[2]==d->target[2] &&
        tuple[0]==ICON32_VG(p,native_graphics_current_device,uint32_t (*)(void))() &&
        tuple[2]==ICON32_VG(p,native_global_target,uint32_t (*)(uint32_t))(p->global_surface_index);
}
static int icon32_unobstructed(FxNativeDisplay *d,const FxNativeDisplayProfile *p,const uint16_t rect[4],int linked) {
    /* 4032988c..40329944 walks direct children at surface+0xc8; the native
     * clip/intersection arguments use the child rectangle at object+0x10. */
    void *list=(uint8_t *)d->surface+0xc8u;
    uint32_t count=ICON32_VG(p,native_list_count,uint32_t (*)(void *))(list);
    if(count>256u)return 0;
    void *cursor=ICON32_VG(p,native_list_begin,void *(*)(void *))(list);
    int found=0;
    for(uint32_t i=0;i<count;i++) {
        uint8_t *child=ICON32_VG(p,native_list_next,void *(*)(void *,void **))(list,&cursor);
        if(child==d->object){found=1;continue;}
        uintptr_t a=(uintptr_t)child;
        if(a<p->ram_begin || a>p->ram_end-0xc0u || (a&7u))return 0;
        if(!ICON32_VG(p,gui_get_visible,uint32_t (*)(void *))(child))continue;
        const int16_t *bounds=(const int16_t *)(const void *)(child+0x10u);
        if(bounds[2]<=bounds[0]||bounds[3]<=bounds[1])return 0;
        if(bounds[0]<rect[2]&&bounds[2]>rect[0]&&bounds[1]<rect[3]&&bounds[3]>rect[1])return 0;
    }
    return found==linked;
}
static uint32_t icon32_float(uint32_t n,uint32_t den) {
    if(!n)return 0;
    int e=(int)__builtin_clz(den)-(int)__builtin_clz(n);
    if(e>=0?(uint64_t)n<((uint64_t)den<<e):((uint64_t)n<<-e)<den)e--;
    uint64_t v=(uint64_t)n<<(23-e),q=v/den,r=v%den;
    if(2*r>den||(2*r==den&&(q&1)))q++;
    if(q==(UINT64_C(1)<<24)){q>>=1;e++;}
    return ((uint32_t)(e+127)<<23)|((uint32_t)q&0x7fffffu);
}
static uint32_t icon32_paint_scene(FxNativeDisplay *d,const FxNativeDisplayProfile *p,int erase,
                                  uintptr_t *last_backing,const FxDemoScene *scene) {
    if(!scene || scene->count<2u || !scene->commands || scene->commands[1].kind!=FX_SCENE_RECT ||
       d->viewport[0]>=d->viewport[2] || d->viewport[1]>=d->viewport[3] ||
       d->viewport[2]>d->width || d->viewport[3]>d->height || d->width>4096u || d->height>4096u)return 0;
    const FxSceneCommand *b=&scene->commands[1];
    if(!b->glyph || !b->scale || (uint32_t)b->x+b->glyph>(uint32_t)d->viewport[2]-d->viewport[0] ||
       (uint32_t)b->y+b->scale>(uint32_t)d->viewport[3]-d->viewport[1])return 0;
    const uint16_t rect[4]={d->viewport[0]+b->x,d->viewport[1]+b->y,
                            d->viewport[0]+b->x+b->glyph,d->viewport[1]+b->y+b->scale};
    if(!icon32_unobstructed(d,p,rect,!erase))return 2;
    ICON32_VG(p,gui_bind_target,void (*)(void *))(d->object);
    ICON32_VG(p,native_graphics_acquire,void (*)(void))();
    uintptr_t backing=ICON32_SELECT(d);
    if(!backing || (erase && backing!=*last_backing)){
        ICON32_VG(p,native_graphics_release,void (*)(void))();
        return 0;
    }
    /* Use SRC on this overlay's bounded footprint. Repeating an antialiased
     * edge, or switching to a darker sweep phase, replaces its previous alpha. */
    (void)ICON32_VG(p,vgGetError,uint32_t (*)(void))();
    uint32_t keys[]={0x1131u,0x1130u,0x1170u,0x1102u,0x1104u,0x1105u,0x1100u};
    const int32_t values[]={0,0,0,1,0x2000,0x1f00,0x1401};
    int32_t old[7];uint32_t changed=0;
    for(uint32_t i=0;i<7u;i++)old[i]=ICON32_VG(p,vgGeti,int32_t (*)(uint32_t))(keys[i]);
    uint32_t error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
    if(error)goto restore;
    for(;changed<7u;changed++)ICON32_VG(p,vgSeti,void (*)(uint32_t,int32_t))(keys[changed],values[changed]);
    error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
    if(error)goto restore;
    uint32_t bottom=d->height-rect[3];
    if(!erase&&scene->count==3u&&scene->commands[2].kind==FX_SCENE_IMAGE) {
        FxSceneImage im;
        uint32_t offset=scene->commands[2].rgb;
        if(!scene->data || offset>scene->data_bytes || sizeof(im)>scene->data_bytes-offset){error=0x1001;goto restore;}
        const uint8_t *descriptor=scene->data+offset;
        for(uint32_t j=0;j<sizeof(im);j++)((uint8_t *)&im)[j]=descriptor[j];
        if(!im.width || !im.height || im.width>4096u || im.height>4096u ||
           im.format!=FX_SCENE_RGBA8 || im.stride<im.width*4u || im.stride>INT32_MAX ||
           im.pixels>scene->data_bytes || (uint64_t)(im.height-1u)*im.stride+im.width*4u>scene->data_bytes-im.pixels){error=0x1001;goto restore;}
        uint32_t handle=ICON32_VG(p,vgCreateImage,uint32_t (*)(uint32_t,int32_t,int32_t,uint32_t))(193,im.width,im.height,1);
        if(handle) {
            ICON32_VG(p,vgImageSubData,void (*)(uint32_t,const void *,int32_t,uint32_t,int32_t,int32_t,int32_t,int32_t))
                (handle,scene->data+im.pixels+(im.height-1u)*im.stride,-(int32_t)im.stride,193,0,0,im.width,im.height);
            error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
            if(!error) {
                uint32_t saved[9],matrix[9]={icon32_float(b->glyph,im.width),0,0,0,icon32_float(b->scale,im.height),0,
                                           icon32_float(rect[0],1),icon32_float(bottom,1),0x3f800000u};
                ICON32_VG(p,vgGetMatrix,void (*)(void *))(saved);
                error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
                if(!error) {
                    ICON32_VG(p,vgLoadMatrix,void (*)(const void *))(matrix);
                    error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
                    if(!error)ICON32_VG(p,vgDrawImage,void (*)(uint32_t))(handle);
                    ICON32_VG(p,vgLoadMatrix,void (*)(const void *))(saved);
                }
            }
            ICON32_VG(p,vgDestroyImage,void (*)(uint32_t))(handle);
        } else error=0x1002u;
    } else {
        /* Independent low-memory text fallback. vgClear replaces pixels and
         * does not allocate an image or use the icon's image upload path. */
        uint32_t saved[4],transparent[4]={0},white[4]={0x3f800000,0x3f800000,0x3f800000,0x3f800000};
        ICON32_VG(p,vgGetfv,void (*)(uint32_t,int32_t,void *))(0x1121u,4,saved);
        error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
        if(error)goto restore;
        ICON32_VG(p,vgSetfv,void (*)(uint32_t,int32_t,const void *))(0x1121u,4,transparent);
        error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
        if(error)goto restore_color;
        ICON32_VG(p,vgClear,void (*)(int32_t,int32_t,int32_t,int32_t))(rect[0],bottom,b->glyph,b->scale);
        ICON32_VG(p,vgSetfv,void (*)(uint32_t,int32_t,const void *))(0x1121u,4,white);
        error=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
        if(error)goto restore_color;
        for(uint32_t i=2;!erase&&i<scene->count;i++) {
            const FxSceneCommand *c=&scene->commands[i];
            if(c->kind!=FX_SCENE_GLYPH||c->x<b->x||c->y<b->y||
               c->x+5u*c->scale>b->x+b->glyph||c->y+7u*c->scale>b->y+b->scale)continue;
            for(uint32_t row=0;row<7;row++) {
                uint32_t bits=fx_glyph((char)c->glyph,row);
                for(uint32_t col=0;col<5;) {
                    if(!(bits&(1u<<(4-col)))){col++;continue;}
                    uint32_t start=col++;
                    while(col<5u&&(bits&(1u<<(4-col))))col++;
                    ICON32_VG(p,vgClear,void (*)(int32_t,int32_t,int32_t,int32_t))
                        (d->viewport[0]+c->x+start*c->scale,
                         d->height-d->viewport[1]-c->y-(row+1u)*c->scale,(col-start)*c->scale,c->scale);
                }
            }
        }
restore_color:
        ICON32_VG(p,vgSetfv,void (*)(uint32_t,int32_t,const void *))(0x1121u,4,saved);
    }
restore:
    for(uint32_t i=changed;i-->0;)ICON32_VG(p,vgSeti,void (*)(uint32_t,int32_t))(keys[i],old[i]);
    uint32_t last=ICON32_VG(p,vgGetError,uint32_t (*)(void))();
    ICON32_VG(p,native_graphics_release,void (*)(void))();
    d->error=error?error:last;
    if(d->error)return 0;
    *last_backing=erase?0:backing;
    if(!erase)d->callbacks++;
    return 1;
}
static inline uint32_t icon32_paint(FxNativeDisplay *d,const FxNativeDisplayProfile *p,int erase,uintptr_t *last_backing) {
    return icon32_paint_scene(d,p,erase,last_backing,d->scene);
}
#endif
