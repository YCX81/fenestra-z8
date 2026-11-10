/* Emulation-only scene composition. PREV.LVP pixels are tightly packed I420,
 * BT.601 limited range. The conversion runs on the playback worker, never in
 * a native draw callback. Native image rendering performs viewport scaling. */
#include "ui.h"
static void scene(FxDemoScene *s,FxSceneCommand *c,uint32_t count,uint8_t *data,uint32_t bytes,uint32_t capacity) {
    s->width=1280u;s->height=800u;s->capacity=PB_SCENE_COMMANDS;
    s->count=count;s->commands=c;s->data=data;s->data_bytes=bytes;s->data_capacity=capacity;
}
static FxSceneCommand command(uint16_t kind,uint16_t x,uint16_t y,uint16_t glyph,uint32_t scale,uint32_t rgb) {
    FxSceneCommand c;c.kind=kind;c.x=x;c.y=y;c.glyph=glyph;c.scale=scale;c.rgb=rgb;return c;
}
FxResult pb_scene_badge(FxDemoScene *s,FxSceneCommand c[PB_SCENE_COMMANDS]) {
    if(!s||!c)return FX_ARGUMENT;
    c[0]=command(FX_SCENE_CLEAR,FX_SCENE_KEEP,0u,0u,0u,0u);
    c[1]=command(FX_SCENE_RECT,24u,24u,112u,44u,0u);
    c[2]=command(FX_SCENE_GLYPH,32u,32u,'L',4u,0xffffffu);
    c[3]=command(FX_SCENE_GLYPH,58u,32u,'I',4u,0xffffffu);
    c[4]=command(FX_SCENE_GLYPH,84u,32u,'V',4u,0xffffffu);
    c[5]=command(FX_SCENE_GLYPH,110u,32u,'E',4u,0xffffffu);
    scene(s,c,6u,0,0u,0u);return FX_OK;
}
FxResult pb_scene_badge_bitmap(FxDemoScene *s,FxSceneCommand c[PB_SCENE_COMMANDS],
    void *data,uint32_t capacity) {
    if(!s||!c||!data)return FX_ARGUMENT;
    if(((uintptr_t)data&3u)||capacity<PB_BADGE_DATA_BYTES)return FX_BOUNDS;
    /* The tiny font is rasterized into retained worker memory. Native drawing
     * receives only a normal RGBA image, matching the verified preview path. */
    static const uint8_t letters[4][7]={
        {16,16,16,16,16,16,31}, {14,4,4,4,4,4,14},
        {17,17,17,17,17,10,4}, {31,16,16,30,16,16,31}};
    uint8_t *out=data;FxSceneImage image;
    image.width=PB_BADGE_WIDTH;image.height=PB_BADGE_HEIGHT;
    image.stride=PB_BADGE_WIDTH*4u;image.format=FX_SCENE_RGBA8;
    image.pixels=sizeof(image);image.reserved=0u;
    for(uint32_t i=0u;i<sizeof(image);++i)out[i]=((const uint8_t *)&image)[i];
    out+=sizeof(image);
    for(uint32_t row=0u;row<PB_BADGE_HEIGHT;++row) {
        for(uint32_t col=0u;col<PB_BADGE_WIDTH;++col) {
            uint8_t color=0u;
            if(row>=8u&&row<36u&&col>=8u) {
                uint32_t letter=(col-8u)/26u,letter_x=(col-8u)%26u;
                if(letter<4u&&letter_x<20u&&
                    (letters[letter][(row-8u)/4u]&(1u<<(4u-letter_x/4u))))color=255u;
            }
            uint32_t pixel=(row*PB_BADGE_WIDTH+col)*4u;
            out[pixel]=out[pixel+1u]=out[pixel+2u]=color;out[pixel+3u]=255u;
        }
    }
    c[0]=command(FX_SCENE_CLEAR,FX_SCENE_KEEP,0u,0u,0u,0u);
    c[1]=command(FX_SCENE_RECT,24u,24u,1u,1u,0u);
    c[2]=command(FX_SCENE_IMAGE,24u,24u,PB_BADGE_WIDTH,PB_BADGE_HEIGHT,0u);
    scene(s,c,3u,data,PB_BADGE_DATA_BYTES,capacity);return FX_OK;
}
static uint8_t clamp(int n) {return n<0?0u:n>255?255u:(uint8_t)n;}
FxResult pb_scene_frame(FxDemoScene *s,FxSceneCommand c[PB_SCENE_COMMANDS],
    void *data,uint32_t capacity,const void *i420,uint32_t bytes) {
    if(!s||!c||!data||!i420)return FX_ARGUMENT;
    uint32_t W,H;
    if(bytes==320u*212u*3u/2u){W=320u;H=212u;}else if(bytes==1024u*682u*3u/2u){W=1024u;H=682u;}else return FX_BOUNDS;
    if(((uintptr_t)data&3u)||capacity<sizeof(FxSceneImage)+W*H*4u)return FX_BOUNDS;
    /* v28: camera held vertically (attitude 1/2) and a pillarboxed portrait frame: only the upright picture */
    uint32_t X0=0u,SW=W;
    if(W==1024u){
        uint32_t att=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bc8004);
        if(att==1u||att==2u){
            const uint8_t *yp=i420;uint32_t dark=1u;
            for(uint32_t r=8u;r<H&&dark;r+=48u)for(uint32_t x=0u;x<280u&&dark;x+=40u)
                if(yp[r*W+x]>24u||yp[r*W+W-1u-x]>24u)dark=0u;
            if(dark){SW=(H*H/W)&~3u;X0=((W-SW)/2u)&~1u;}
        }
    }
    uint8_t *out=data;
    /* Byte-wise header copy also permits the input frame itself to be unaligned. */
    FxSceneImage image;
    image.width=SW;image.height=H;
    image.stride=SW*4u;image.format=FX_SCENE_RGBA8;
    image.pixels=sizeof(image);image.reserved=0u;
    for(uint32_t i=0u;i<sizeof(image);++i)out[i]=((const uint8_t *)&image)[i];
    const uint8_t *y=i420,*u=y+W*H;
    const uint8_t *v=u+W*H/4u;
    out+=sizeof(image);
    for(uint32_t row=0u;row<H;++row) {
        for(uint32_t oc=0u;oc<SW;++oc) {
            uint32_t col=oc+X0;
            uint32_t uv=(row/2u)*(W/2u)+col/2u;
            int yy=(int)y[row*W+col]-16;
            int uu=(int)u[uv]-128,vv=(int)v[uv]-128;
            if(yy<0)yy=0;
            uint32_t pixel=(row*SW+oc)*4u;
            out[pixel]=clamp((298*yy+409*vv+128)>>8);
            out[pixel+1u]=clamp((298*yy-100*uu-208*vv+128)>>8);
            out[pixel+2u]=clamp((298*yy+516*uu+128)>>8);
            out[pixel+3u]=255u;
        }
    }
    c[0]=command(FX_SCENE_CLEAR,FX_SCENE_KEEP,0u,0u,0u,0u);
    c[1]=command(FX_SCENE_RECT,0u,0u,1280u,800u,0u);
    c[2]=command(FX_SCENE_IMAGE,0u,0u,1280u,800u,0u);   /* the scene already spans the 3:2 photo area */
    scene(s,c,3u,data,(uint32_t)(sizeof(FxSceneImage)+SW*H*4u),capacity);return FX_OK;
}
