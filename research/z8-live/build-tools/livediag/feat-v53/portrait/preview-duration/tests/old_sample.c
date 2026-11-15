#include "live_stream.h"
#include "preview_writer.h"
#include <string.h>
int live_stream_preview_frame(void *stream,uint32_t index,void *out,size_t bytes) {
 struct live_stream *s=stream;
 if(!s||!s->preview||!s->preview_count||!out||bytes!=LIVE_PREVIEW_FRAME)return -1;
 uint64_t target=s->clip_start+(uint64_t)index*LIVE_PREVIEW_STEP_US;
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
