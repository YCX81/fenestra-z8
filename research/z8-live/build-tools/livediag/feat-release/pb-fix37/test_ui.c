/* Host-only test: scene validity, known BT.601 colors and rejection safety. */
#include "ui.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
void *fx_memcpy(void *d,const void *s,size_t n) {return memcpy(d,s,n);}
static void fill(uint8_t *frame,uint8_t y,uint8_t u,uint8_t v) {
    memset(frame,y,320u*212u);memset(frame+320u*212u,u,320u*212u/4u);
    memset(frame+320u*212u*5u/4u,v,320u*212u/4u);
}
static void color(const uint8_t *data,unsigned r,unsigned g,unsigned b) {
    const uint8_t *p=data+sizeof(FxSceneImage);
    for(unsigned i=0;i<320u*212u;i++) {
        assert(abs((int)p[4*i]-(int)r)<=1);assert(abs((int)p[4*i+1]-(int)g)<=1);
        assert(abs((int)p[4*i+2]-(int)b)<=1);assert(p[4*i+3]==255);
    }
}
int main(void) {
    FxSceneCommand cmd[PB_SCENE_COMMANDS];FxDemoScene s;
    uint8_t *frame=malloc(PB_PREVIEW_FRAME_BYTES),*data=malloc(PB_SCENE_DATA_BYTES);
    assert(frame&&data);assert(pb_scene_badge(&s,cmd)==FX_OK);assert(fx_scene_validate(&s)==FX_OK);
    assert(s.count==6&&cmd[0].x==FX_SCENE_KEEP&&cmd[2].glyph=='L'&&cmd[5].glyph=='E');
    assert(pb_scene_badge_bitmap(&s,cmd,data,PB_SCENE_DATA_BYTES)==FX_OK);
    assert(fx_scene_validate(&s)==FX_OK&&s.count==3&&cmd[2].kind==FX_SCENE_IMAGE);
    FxSceneImage badge;memcpy(&badge,data,sizeof(badge));
    assert(badge.width==PB_BADGE_WIDTH && badge.height==PB_BADGE_HEIGHT && badge.stride==4u*PB_BADGE_WIDTH);
    assert(cmd[2].x==ICON32_PB_X && cmd[2].y==ICON32_PB_Y);
    assert(cmd[2].glyph==ICON32_PB_DEST_WIDTH && cmd[2].scale==ICON32_PB_DEST_HEIGHT);
    for(unsigned i=0;i<PB_BADGE_WIDTH*PB_BADGE_HEIGHT;i++) {
        const uint8_t *p=data+sizeof(badge)+i*4u;
        Icon32Asset asset=icon32_pb_default();uint8_t rgba[4];
        icon32_pb_rgba(&asset,i,rgba);assert(memcmp(p,rgba,4u)==0);
    }
    memset(data,0xa5,PB_SCENE_DATA_BYTES);
    assert(pb_scene_badge_bitmap(&s,cmd,data,PB_BADGE_DATA_BYTES-1)==FX_BOUNDS);
    for(unsigned i=0;i<PB_SCENE_DATA_BYTES;i++)assert(data[i]==0xa5);
    fill(frame,16,128,128);assert(pb_scene_frame(&s,cmd,data,PB_SCENE_DATA_BYTES,frame,320u*212u*3u/2u)==FX_OK);
    assert(fx_scene_validate(&s)==FX_OK);color(data,0,0,0);
    fill(frame,235,128,128);assert(pb_scene_frame(&s,cmd,data,PB_SCENE_DATA_BYTES,frame,320u*212u*3u/2u)==FX_OK);color(data,255,255,255);
    fill(frame,81,90,240);assert(pb_scene_frame(&s,cmd,data,PB_SCENE_DATA_BYTES,frame,320u*212u*3u/2u)==FX_OK);color(data,255,0,0);
    /* Neighboring 2x2 blocks consume independent chroma samples. */
    fill(frame,81,128,128);frame[320u*212u]=90;frame[320u*212u*5u/4u]=240;
    assert(pb_scene_frame(&s,cmd,data,PB_SCENE_DATA_BYTES,frame,320u*212u*3u/2u)==FX_OK);
    assert(data[sizeof(FxSceneImage)]>=254&&data[sizeof(FxSceneImage)+8]<100);
    memset(data,0xa5,PB_SCENE_DATA_BYTES);
    assert(pb_scene_frame(&s,cmd,data,PB_SCENE_DATA_BYTES,frame,PB_PREVIEW_FRAME_BYTES-1)==FX_BOUNDS);
    for(unsigned i=0;i<PB_SCENE_DATA_BYTES;i++)assert(data[i]==0xa5);
    assert(pb_scene_frame(&s,cmd,data,PB_SCENE_DATA_BYTES-1,frame,PB_PREVIEW_FRAME_BYTES)==FX_BOUNDS);
    assert(pb_scene_frame(&s,cmd,data+1,PB_SCENE_DATA_BYTES,frame,PB_PREVIEW_FRAME_BYTES)==FX_BOUNDS);
    assert(pb_scene_frame(&s,cmd,data,PB_SCENE_DATA_BYTES,0,PB_PREVIEW_FRAME_BYTES)==FX_ARGUMENT);
    free(frame);free(data);puts("{\"badge_scene_validate\":true,\"badge_bitmap_validate\":true,\"badge_pixels_match\":true,\"frame_scene_validate\":true,\"black_white_red\":true,\"chroma_2x2\":true,\"reject_before_write\":true}");return 0;
}
