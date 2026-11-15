/* Emulation-only scene composition. PREV.LVP pixels are tightly packed I420,
 * BT.601 limited range. The conversion runs on the playback worker, never in
 * a native draw callback. Native image rendering performs viewport scaling. */
#include "ui.h"
#include "icon32_pb.h"
static void scene(FxDemoScene *s,FxSceneCommand *c,uint32_t count,uint8_t *data,uint32_t bytes,uint32_t capacity) {
    s->width=1280u;s->height=800u;s->capacity=PB_SCENE_COMMANDS;
    s->count=count;s->commands=c;s->data=data;s->data_bytes=bytes;s->data_capacity=capacity;
}
static FxSceneCommand command(uint16_t kind,uint16_t x,uint16_t y,uint16_t glyph,uint32_t scale,uint32_t rgb) {
    FxSceneCommand c;c.kind=kind;c.x=x;c.y=y;c.glyph=glyph;c.scale=scale;c.rgb=rgb;return c;
}
void pb_icon32_label(FxDemoScene *s,FxSceneCommand *c,uint32_t x,uint32_t y,uint32_t w,uint32_t h) {
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
FxResult pb_scene_badge_bitmap(FxDemoScene *s,FxSceneCommand c[PB_SCENE_COMMANDS],
    void *data,uint32_t capacity) {
    if(!s||!c||!data)return FX_ARGUMENT;
    if(((uintptr_t)data&3u)||capacity<PB_BADGE_DATA_BYTES)return FX_BOUNDS;
    /* The tiny font is rasterized into retained worker memory. Native drawing
     * receives only a normal RGBA image, matching the verified preview path. */
    uint8_t *out=data;FxSceneImage image;
    image.width=PB_BADGE_WIDTH;image.height=PB_BADGE_HEIGHT;
    image.stride=PB_BADGE_WIDTH*4u;image.format=FX_SCENE_RGBA8;
    image.pixels=sizeof(image);image.reserved=0u;
    for(uint32_t i=0u;i<sizeof(image);++i)out[i]=((const uint8_t *)&image)[i];
    out+=sizeof(image);
    Icon32Asset asset=icon32_pb_default();
    for(uint32_t i=0;i<PB_BADGE_WIDTH*PB_BADGE_HEIGHT;i++)icon32_pb_rgba(&asset,i,out+4u*i);
    c[0]=command(FX_SCENE_CLEAR,FX_SCENE_KEEP,0u,0u,0u,0u);
    c[1]=command(FX_SCENE_RECT,ICON32_PB_X,ICON32_PB_Y,1u,1u,0u);
    c[2]=command(FX_SCENE_IMAGE,ICON32_PB_X,ICON32_PB_Y,ICON32_PB_DEST_WIDTH,ICON32_PB_DEST_HEIGHT,0u);
    scene(s,c,3u,data,PB_BADGE_DATA_BYTES,capacity);return FX_OK;
}
/* v61: the native movie page owns video rendering. */
#include "runtime.h"
#include "icon32_layout.h"
#include "fenestra_memory.h"
/* Called under the owner GUI mutex, after viewport capture. Both canonical
 * worker storage and this GUI-owned command copy outlive native callbacks. */
const FxDemoScene *pb_icon32_scene(FxNativeDisplay *display,const FxDemoScene *source) {
    int text=source->count>=2u && !source->data && source->commands[0].x==FX_SCENE_KEEP;
    if(!text && (source->count!=3u || source->commands[1].glyph!=1u ||
       source->commands[2].kind!=FX_SCENE_IMAGE))return source; /* LVP frame */
    PbRuntime *r=pb_runtime_get();
    if(!r||display!=&r->display.display||source!=&r->scene)return source;
    uint32_t vw=display->viewport[2]-display->viewport[0];
    uint32_t vh=display->viewport[3]-display->viewport[1];
    const uint32_t *target=display->target;
    Icon32Asset asset=icon32_pb_select(target[0],target[1],target[2]);
    Icon32Rect b=icon32_rect(vw,vh,1280u,800u,asset.x,asset.y,asset.dw,asset.dh);
    r->badge_scene=*source;
#if ICON32_PB_HAS_TARGETS
    /* The worker owns the allocation (>2 MiB for LVP). While request==3
     * it cannot start composing another scene. Its default badge occupies
     * only the prefix; variant bytes live in the disjoint, aligned tail. */
    _Static_assert(PB_BADGE_DATA_BYTES+sizeof(FxSceneImage)+ICON32_PB_MAX_PIXELS*4u
                   <=PB_SCENE_DATA_BYTES,"target-specific badge exceeds reserved pixels");
    if(!text && asset.mask) {
        uint32_t pixels=(uint32_t)asset.w*asset.h;
        FxSceneImage im={asset.w,asset.h,(uint32_t)asset.w*4u,FX_SCENE_RGBA8,sizeof(im),0u};
        uint8_t *dst=r->pixels+PB_BADGE_DATA_BYTES;
        memcpy(dst,&im,sizeof(im));
        for(uint32_t i=0;i<pixels;i++) {
            icon32_pb_rgba(&asset,i,dst+sizeof(im)+4u*i);
        }
        r->badge_scene.data=dst;
        r->badge_scene.data_bytes=r->badge_scene.data_capacity=sizeof(im)+pixels*4u;
    }
#endif
    if(text){
        pb_icon32_label(&r->badge_scene,r->badge_commands,b.x,b.y,b.w,b.h);
        r->badge_scene.width=vw;r->badge_scene.height=vh;
        return &r->badge_scene;
    }
    memcpy(r->badge_commands,source->commands,3u*sizeof(*r->badge_commands));
    r->badge_scene.commands=r->badge_commands;r->badge_scene.capacity=3u;
    r->badge_scene.width=vw;r->badge_scene.height=vh;
    r->badge_commands[1].x=r->badge_commands[2].x=b.x;
    r->badge_commands[1].y=r->badge_commands[2].y=b.y;
    r->badge_commands[1].glyph=r->badge_commands[2].glyph=b.w;
    r->badge_commands[1].scale=r->badge_commands[2].scale=b.h;
    return &r->badge_scene;
}
