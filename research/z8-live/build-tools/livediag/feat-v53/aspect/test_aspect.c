/* Execute the real PB converter and OpenVG submission code. Native drawing is
 * captured at its API boundary; viewport fixtures are not camera screenshots. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "work/pb-fix37/ui.h"
#ifndef PB_NATIVE_SOURCE
#define PB_NATIVE_SOURCE "work/pb-fix37/native_ui.c"
#endif
#include PB_NATIVE_SOURCE
#undef memcpy
#undef memset
#undef memcmp
#undef memmove
static uint32_t submitted[9];static unsigned created,destroyed,drawn,loads;
void *fx_memcpy(void *a,const void *b,size_t n){return memcpy(a,b,n);}
void *fx_memset(void *a,int b,size_t n){return memset(a,b,n);}
int fx_memcmp(const void *a,const void *b,size_t n){return memcmp(a,b,n);}
static uint32_t create(uint32_t fmt,int32_t w,int32_t h,uint32_t quality){
 assert(fmt==193&&w>0&&h>0&&quality==1);created++;return 7;
}
static void destroy(uint32_t id){assert(id==7);destroyed++;}
static void pixels(uint32_t id,const void *p,int32_t stride,uint32_t fmt,
 int32_t x,int32_t y,int32_t w,int32_t h){
 assert(id==7&&p&&stride==-w*4&&fmt==193&&!x&&!y&&h>0);
}
static uint32_t noerror(void){return 0;}
static void set(uint32_t k,int32_t v){assert(k==0x1100&&(v==0x1401||v==0x1400));}
static void getmatrix(void *p){memset(p,0,36);}
static void matrix(const void *p){if(!(loads++&1))memcpy(submitted,p,36);}
static void drawimage(uint32_t id){assert(id==7);drawn++;}
#define P(f) ((uintptr_t)(f))
const FxNativeDisplayProfile pb_native_display_profile={
 .vgCreateImage=P(create),.vgDestroyImage=P(destroy),.vgImageSubData=P(pixels),
 .vgGetError=P(noerror),.vgSeti=P(set),.vgGetMatrix=P(getmatrix),
 .vgLoadMatrix=P(matrix),.vgDrawImage=P(drawimage)
};
static double number(uint32_t n){float f;memcpy(&f,&n,4);return f;}
static void fit(PbNativeDisplay *p,const FxDemoScene *s,
 unsigned x,unsigned y,unsigned w,unsigned h,unsigned surface_h){
 FxNativeDisplay *d=&p->display;d->scene=s;d->height=surface_h;d->width=x+w;
 d->viewport[0]=x;d->viewport[1]=y;d->viewport[2]=x+w;d->viewport[3]=y+h;
 assert(image(d,s->commands+2)==0);
 const FxSceneImage *im=(const void*)s->data;
 double sx=number(submitted[0]),sy=number(submitted[4]);
 double left=number(submitted[6]),bottom=number(submitted[7]);
 double right=left+sx*im->width,top=bottom+sy*im->height;
 /* Circles stay circular to within one destination pixel; no crop. */
 assert(fabs(sx-sy)*im->width<1.6);
 assert(left>=x-0.001&&right<=x+w+0.001);
 assert(bottom>=surface_h-y-h-0.001&&top<=surface_h-y+0.001);
 assert(fabs((left+right)/2.0-(x+w/2.0))<=0.501);
 assert(fabs((top+bottom)/2.0-(surface_h-y-h/2.0))<=0.501);
 assert(fabs(right-left-w)<0.001||fabs(top-bottom-h)<0.001);
}
int main(void){
 uint8_t *src=malloc(PB_PREVIEW_FRAME_BYTES),*dst=malloc(PB_SCENE_DATA_BYTES);
 assert(src&&dst);memset(src,16,1024u*682u);memset(src+1024u*682u,128,1024u*682u/2u);
 FxDemoScene s;FxSceneCommand cmd[PB_SCENE_COMMANDS];PbNativeDisplay p={0};
 assert(pb_scene_frame(&s,cmd,dst,PB_SCENE_DATA_BYTES,src,PB_PREVIEW_FRAME_BYTES)==FX_OK);
 FxSceneImage *im=(void*)dst;assert(im->width==1024&&im->height==682);
 /* A dark landscape must not become a portrait crop merely because the body
  * is held vertically. No camera attitude read is needed for any frame. */
 for(unsigned i=0;i<1024u*682u;i++)assert(dst[sizeof(*im)+4*i]==0);
 fit(&p,&s,0,0,1280,800,800);fit(&p,&s,0,0,800,1280,1280);
 fit(&p,&s,0,0,1280,960,960);fit(&p,&s,0,0,960,1280,1280);
 fit(&p,&s,48,64,480,800,960);fit(&p,&s,64,48,800,480,600);
 assert(pb_scene_frame(&s,cmd,dst,PB_SCENE_DATA_BYTES,src,320u*212u*3u/2u)==FX_OK);
 fit(&p,&s,0,0,720,480,480);fit(&p,&s,0,0,480,720,720);
 /* Ordinary badge command retains its explicitly requested rectangle. */
 assert(pb_scene_badge_bitmap(&s,cmd,dst,PB_SCENE_DATA_BYTES)==FX_OK);
 p.display.scene=&s;p.display.height=800;p.display.viewport[0]=0;p.display.viewport[1]=0;
 p.display.viewport[2]=1280;p.display.viewport[3]=800;
 assert(image(&p.display,cmd+2)==0);im=(void*)dst;
 assert(fabs(number(submitted[0])*im->width-cmd[2].glyph)<0.001);
 assert(fabs(number(submitted[4])*im->height-cmd[2].scale)<0.001);
 assert(created==9&&destroyed==9&&drawn==9&&loads==18);
 free(src);free(dst);puts("PASS: 8 frame viewports, dark landscape retained, badge unchanged, VG ownership balanced");
}
