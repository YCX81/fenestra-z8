#include "preview_writer.h"
static void le16(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void le32(uint8_t *p, uint32_t v) { le16(p,v); le16(p+2,v>>16); }
static int put(const LivePreviewWriter *w, const void *p, size_t n) {
    size_t done=0;
    return w->write(w->ctx,p,n,&done) || done!=n ? -1 : 0;
}
int live_preview_write(const LivePreviewWriter *w, void *scratch, size_t bytes) {
    uint8_t h[32]={0};
    if(!w||!w->frame||!w->write||!scratch||bytes<LIVE_PREVIEW_FRAME_BYTES) return -1;
    le32(h,0x3150564c); le16(h+4,1); le16(h+6,32);
    le16(h+8,LIVE_PREVIEW_WIDTH); le16(h+10,LIVE_PREVIEW_HEIGHT);
    le32(h+12,LIVE_PREVIEW_FRAMES); le32(h+16,LIVE_PREVIEW_FRAME_US);
    le32(h+20,LIVE_PREVIEW_FRAME_BYTES);
    le32(h+24,LIVE_PREVIEW_FRAME_BYTES*LIVE_PREVIEW_FRAMES);
    if(put(w,h,sizeof h)) return -1;
    for(uint32_t i=0;i<LIVE_PREVIEW_FRAMES;i++) {
        if(w->frame(w->ctx,i,scratch,LIVE_PREVIEW_FRAME_BYTES)||
           put(w,scratch,LIVE_PREVIEW_FRAME_BYTES)) return -1;
        if(w->yield) w->yield(w->ctx);
    }
    return 0;
}
