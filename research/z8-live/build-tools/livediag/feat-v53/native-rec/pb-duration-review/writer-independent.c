
#include "live_stream.h"
#include "preview_writer.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int live_stream_preview_frame(void *stream,uint32_t index,void *out,size_t bytes) {
 struct live_stream *s=stream;
 if(!s||!s->preview||!s->preview_count||!out||bytes!=LIVE_PREVIEW_FRAME||index>=LIVE_PREVIEW_FRAMES)return -1;
 uint64_t span=s->clip_end-s->clip_start;if(!span||span>4000000u)return -1;
 /* The IDR-aligned clip is not always three seconds. Sample its actual
  * frozen interval, matching the duration recorded in the LVJ1 header. */
 uint64_t target=s->clip_start+span*index/LIVE_PREVIEW_FRAMES;
 unsigned oldest=(s->preview_head+LIVE_PREVIEW_SLOTS-s->preview_count)%LIVE_PREVIEW_SLOTS,pick=oldest;
 for(unsigned i=0;i<s->preview_count;i++){unsigned k=(oldest+i)%LIVE_PREVIEW_SLOTS;if(s->preview_pts[k]<=target)pick=k;else break;}
 const uint8_t *src=s->preview+(size_t)pick*LIVE_PREVIEW_FRAME;unsigned o=s->preview_orient;
 if(o!=6&&o!=8&&o!=3){memcpy(out,src,LIVE_PREVIEW_FRAME);return 0;}
 /* v13: upright preview. 3: 180 degrees, full frame. 6/8: rotate 90 cw/ccw, fit CWxH centred, black bars. */
 enum{W=LIVE_PREVIEW_W,H=LIVE_PREVIEW_H,CW=(H*H/W)&~3u,X0=((W-CW)/2)&~1u};
 uint8_t *dy=out,*du=dy+W*H,*dv=du+W*H/4;const uint8_t *sy=src,*su=sy+W*H,*sv=su+W*H/4;
 for(unsigned pl=0;pl<3;pl++){unsigned w=pl?W/2:W,h=pl?H/2:H,cw=pl?CW/2:CW,x0=pl?X0/2:X0;
  const uint8_t *sp=pl==0?sy:pl==1?su:sv;uint8_t *dp=pl==0?dy:pl==1?du:dv;uint8_t fill=pl?128:16;
  for(unsigned y=0;y<h;y++){uint8_t *row=dp+(size_t)y*w;
   if(o==3){const uint8_t *sr=sp+(size_t)(h-1-y)*w;for(unsigned x=0;x<w;x++)row[x]=sr[w-1-x];continue;}
   uint32_t ryf=(uint32_t)(((uint64_t)y*w<<8)/h),mx=(w-1u)<<8,my=(h-1u)<<8; /* v20: 24.8 fixed point */
   if(ryf>mx)ryf=mx;
   for(unsigned x=0;x<w;x++){
    if(x<x0||x>=x0+cw){row[x]=fill;continue;}
    uint32_t rxf=(uint32_t)(((uint64_t)(x-x0)*h<<8)/cw);if(rxf>my)rxf=my;
    uint32_t sxf=o==6?ryf:mx-ryf,syf=o==6?my-rxf:rxf;
    unsigned ix=sxf>>8,iy=syf>>8,fx=sxf&255u,fy=syf&255u,ix1=ix+1u<w?ix+1u:ix,iy1=iy+1u<h?iy+1u:iy;
    const uint8_t *r0=sp+(size_t)iy*w,*r1=sp+(size_t)iy1*w;
    uint32_t top=r0[ix]*(256u-fx)+r0[ix1]*fx,bot=r1[ix]*(256u-fx)+r1[ix1]*fx;
    row[x]=(uint8_t)((top*(256u-fy)+bot*fy+32768u)>>16);}}}
 return 0;
}
int main(void){
 struct live_stream*s=calloc(1,sizeof *s);uint8_t*out=malloc(LIVE_PREVIEW_FRAME_BYTES);
 s->preview=malloc((size_t)LIVE_PREVIEW_FRAME*LIVE_PREVIEW_SLOTS);assert(s&&out&&s->preview);
 s->preview_count=LIVE_PREVIEW_SLOTS;s->preview_head=17;
 uint32_t spans[]={1499941,1499985,3069729,3069733,3533866,3999960};unsigned cases=0;
 for(unsigned large=0;large<2;large++)for(unsigned c=0;c<6;c++){
  uint64_t d=spans[c];s->clip_start=large?UINT64_MAX-4000001:100000000;
  s->clip_end=s->clip_start+d;
  for(unsigned j=0;j<LIVE_PREVIEW_SLOTS;j++){unsigned k=(17+j)%LIVE_PREVIEW_SLOTS;
   s->preview_pts[k]=s->clip_start+d*j/LIVE_PREVIEW_SLOTS;
   memset(s->preview+(size_t)k*LIVE_PREVIEW_FRAME,16+j,LIVE_PREVIEW_FRAME);}
  for(unsigned q=0;q<LIVE_PREVIEW_FRAMES;q++){
   assert(!live_stream_preview_frame(s,q,out,LIVE_PREVIEW_FRAME));
   uint64_t target=s->clip_start+d*q/LIVE_PREVIEW_FRAMES;assert(target>=s->clip_start&&target<s->clip_end);
   unsigned pick=0;for(unsigned j=0;j<LIVE_PREVIEW_SLOTS;j++)if(s->preview_pts[(17+j)%LIVE_PREVIEW_SLOTS]<=target)pick=j;
   assert(out[0]==16+pick);
  }cases++;
 }
 /* Exhaust every integer duration admitted by the unchanged reader cap.
  * Includes writer lower rounding boundary, not just rounded test spans. */
 uint32_t admitted=0;
 for(uint32_t d=0;d<=4000000;d++){
  uint32_t dt=(uint32_t)(((uint64_t)d+LIVE_PREVIEW_FRAMES-1)/LIVE_PREVIEW_FRAMES);
  if(dt>=33333&&dt<=4000000/LIVE_PREVIEW_FRAMES){
   uint64_t shown=(uint64_t)dt*LIVE_PREVIEW_FRAMES;
   assert(shown>=d&&shown-d<45&&shown<=4000000);
   assert(d>=1499941&&d<=3999960);admitted++;
  }
 }
 free(s->preview);free(out);free(s);
 printf("{\"passed\":true,\"sample_cases\":%u,\"admitted_integer_spans\":%u}\n",cases,admitted);
}
