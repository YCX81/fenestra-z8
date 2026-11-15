#include "live_codec.h"
#define H264E_SVC_API 0
#define H264E_MAX_THREADS 0
#define H264E_ENABLE_DENOISE 0
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"
#ifndef LIVE_ENCODE_SPEED
#define LIVE_ENCODE_SPEED 9
#endif
_Static_assert(LIVE_ENCODE_SPEED>=0 && LIVE_ENCODE_SPEED<=10,"minih264 speed");
static H264E_create_param_t params(unsigned w,unsigned h) {
 H264E_create_param_t p={0};p.width=w;p.height=h;p.gop=90;
 /* Upstream chooses SPS level from VBV, FS, DPB but not macroblocks/sec.
    600000 > Level2.2 CPB selects Level3.0 for 640x424 @ <=30fps. */
 p.vbv_size_bytes=1000000;p.const_input_flag=1;p.enableNEON=1;
 return p;
}
int live_encoder_sizes(unsigned w,unsigned h,size_t *persist,size_t *scratch){
 int a,b,r;H264E_create_param_t p=params(w,h);
 if(!persist||!scratch||!w||!h||(w&1)||(h&1))return -1;
 r=H264E_sizeof(&p,&a,&b);if(r)return r;*persist=a;*scratch=b;return 0;
}
int live_encoder_init(live_encoder *e,void *p,void *s,unsigned w,unsigned h){
 size_t a,b;H264E_create_param_t c=params(w,h);int r;
 if(!e||!p||!s||((uintptr_t)p&15)||((uintptr_t)s&15)||live_encoder_sizes(w,h,&a,&b))return -1;
 r=H264E_init(p,&c);if(r)return r;
 e->persist=p;e->scratch=s;e->width=w;e->height=h;e->frames=0;e->last_pts=0;return 0;
}
int live_encoder_frame(live_encoder *e,const uint8_t *in,const uint8_t **out,size_t *bytes){
 H264E_run_param_t r={0};H264E_io_yuv_t y;unsigned plane;unsigned char *o;int n,rc;
 if(!e||!in||!out||!bytes||((uintptr_t)in&15))return -1;
 plane=e->width*e->height;y.yuv[0]=(uint8_t*)in;y.yuv[1]=(uint8_t*)in+plane;y.yuv[2]=(uint8_t*)in+plane+plane/4;
 y.stride[0]=e->width;y.stride[1]=y.stride[2]=e->width/2;
 r.encode_speed=LIVE_ENCODE_SPEED;r.frame_type=e->frames?H264E_FRAME_TYPE_P:H264E_FRAME_TYPE_KEY;
 r.desired_frame_bytes=10000;r.qp_min=20;r.qp_max=40;
 rc=H264E_encode(e->persist,e->scratch,&r,&y,&o,&n);if(rc)return rc;
 *out=o;*bytes=n;e->frames++;return 0;
}

/* Backend boundary: I420 plus timing/rate policy in, independent Annex-B AU out.
 * KEY emits SPS/PPS and IDR, so every selected ring IDR is self-contained. */
int live_encoder_frame_timed(live_encoder *e,const uint8_t *in,uint64_t pts,
 uint64_t *last_idr,unsigned bitrate,unsigned interval,const uint8_t **out,
 size_t *bytes,unsigned *idr) {
 H264E_run_param_t r={0};H264E_io_yuv_t y;unsigned char *o;int n,rc;
 if(!e||!in||!out||!bytes||!last_idr||!idr||!bitrate||!interval||((uintptr_t)in&15))return -1;
 unsigned plane=e->width*e->height;
 y.yuv[0]=(uint8_t *)in;y.yuv[1]=(uint8_t *)in+plane;y.yuv[2]=(uint8_t *)in+plane+plane/4;
 y.stride[0]=e->width;y.stride[1]=y.stride[2]=e->width/2;
 *idr=!e->frames || pts-*last_idr>=interval;
 r.encode_speed=LIVE_ENCODE_SPEED;r.frame_type=*idr?H264E_FRAME_TYPE_KEY:H264E_FRAME_TYPE_P;
 /* Budget follows real accepted PTS; bounded delta avoids a stall creating
  * an unbounded burst. This is a VBR target, not a hard per-AU cap. */
 uint64_t dt=e->frames?pts-e->last_pts:33333;
 if(dt>100000)dt=100000;
 r.desired_frame_bytes=(int)((uint64_t)bitrate*dt/8000000);
 r.qp_min=20;r.qp_max=45;
 rc=H264E_encode(e->persist,e->scratch,&r,&y,&o,&n);if(rc)return rc;
 *out=o;*bytes=n;e->frames++;e->last_pts=pts;if(*idr)*last_idr=pts;return 0;
}
