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
