/* Host replay of the Live capture/stream state machine with the hardware timeline measured on
 * the Z8 (2026-11-05): 30 fps live view in 6 rotating buffers, 27 ms copy, ~194 ms encode,
 * shutter -> C0 stop/hold + still-pool cancels, live-view pause, geometry change, pool gate.
 * Built against the patched sources with the aarch64 burst asm replaced by memcpy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "capture/live_stream.h"
#include "capture/live_capture.h"
#include "codec/au_ring.h"
static uint64_t T;                     /* simulated microseconds */
static uint64_t clock_us(void){return T;}
static int pool_gate=0,geo_w=1024,geo_h=682;
static uint64_t latest_seq;            /* irq_seq of the newest completed frame */
static uint8_t frames[6][1024*682*3/2];
static struct la_capture *CAP;
static int alloc_cb(void *x,uint32_t n,void **d,uint8_t **p,uint32_t *a){(void)x;if(pool_gate)return 0;*p=malloc(n);*d=*p;*a=n;return 1;}
static void release_cb(void *x,void *d){(void)x;free(d);}
static int begin_cb(void *x,uint64_t gen,uint64_t ts,struct la_source *s){(void)x;
 unsigned b=(unsigned)(latest_seq/2)%6;
 *s=(struct la_source){frames[b],frames[b]+1024*682,(uint32_t)geo_w,(uint32_t)geo_h,1024,1024,gen,ts,CAP->cancel_epoch};return 1;}
static int valid_cb(void *x,const struct la_source *s){(void)x;unsigned b=(unsigned)(latest_seq/2)%6;
 return CAP->irq_seq==s->generation*2 && CAP->cancel_epoch==s->lease && frames[b]==s->y;}
static void end_cb(void *x,const struct la_source *s){(void)x;(void)s;}
/* encoder stub: takes enc_us of simulated time, IDR every interval */
static uint64_t enc_us=194000;static uint8_t au[64];
int live_encoder_init(live_encoder *e,void *p,void *t,unsigned w,unsigned h){(void)p;(void)t;e->width=w;e->height=h;return 0;}
void live_encoder_set_flags(live_encoder *e,unsigned f){(void)e;(void)f;}
int live_encoder_sizes(unsigned w,unsigned h,size_t *p,size_t *t){(void)w;(void)h;*p=4096;*t=4096;return 0;}
int live_encoder_frame_timed(live_encoder *e,const uint8_t *in,uint64_t pts,uint64_t *last_idr,unsigned br,unsigned interval,const uint8_t **out,size_t *n,unsigned *idr){
 (void)e;(void)in;(void)br;T+=enc_us;*idr=!*last_idr||pts-*last_idr>=interval;if(*idr)*last_idr=pts;*out=au;*n=sizeof au;return 0;}
static void frame_irq(void){la_irq_signal(CAP,0x41070030u,1,T);latest_seq=CAP->irq_seq;}
struct scen{const char *name;int soft;uint64_t cancel_delay,pause,geo_frames;int gate_ms;};
static int run(struct scen sc,int verbose){
 static struct la_capture c;static struct live_stream s;memset(&c,0,sizeof c);CAP=&c;
 struct la_ops ops={&c,alloc_cb,release_cb,begin_cb,valid_cb,end_cb};
 la_init(&c,&ops);live_stream_init(&s);s.tune.soft_cancel=sc.soft;la_request(&c,1);
 T=1000000;latest_seq=0;pool_gate=0;geo_w=1024;geo_h=682;
 uint64_t next_frame=T,press_at=6000000,lv_pause_end=0,geo_until=0;int pressed=0,accepted=0,froze=0,cancels=0;
 unsigned ev_seen=0;
 for(int it=0;it<200000 && T<14000000;it++){
  /* deliver all frames due by now (live view runs independently of the worker) */
  while(next_frame<=T){
   if(!(lv_pause_end && next_frame<lv_pause_end)){
    if(geo_until && next_frame<geo_until){geo_w=720;geo_h=480;}else{geo_w=1024;geo_h=682;}
    frame_irq();}
   next_frame+=33367;}
  if(!pressed && T>=press_at && c.phase==LA_READY){
   pressed=1;la_full_press(&c,T);
   /* shutter: C0 stop/hold + still pool cancel shortly after, live view paused, geometry changes */
   uint64_t t0=T;T+=sc.cancel_delay;la_cancel(&c);la_cancel(&c);la_cancel(&c);cancels+=3;T=t0;
   lv_pause_end=T+sc.pause;geo_until=lv_pause_end+sc.geo_frames*33367;
   if(sc.gate_ms){pool_gate=1;}
  }
  if(pool_gate && T>press_at+(uint64_t)sc.gate_ms*1000)pool_gate=0;
  uint64_t before=T;
  live_stream_step(&c,&s,T,1,clock_us);
  if(T==before)T+=6000; else T+=27000+6000; /* copy cost + sleep */
  if(c.accepted_presses)accepted=1;
  if(s.job==2&&!froze){froze=1;if(verbose)printf("  froze %u AUs, %u bytes at t=%.2fs\n",s.frozen_count,s.frozen_size,T/1e6);
   s.job=4;} /* the mux would run here */
 }
 if(verbose){unsigned n=s.log_head<LIVE_LOG?s.log_head:LIVE_LOG;
  for(unsigned i=ev_seen;i<n;i++){struct live_ev *e=&s.log[i];printf("  ev t=%.3f type=%u a=%u b=%llu\n",e->t/1e6,e->type,e->a,(unsigned long long)e->b);}}
 printf("%-28s soft=%d accepted=%d frozen=%d phase=%u enc=%llu soft_cancel=%llu short=%llu geo_skip=%llu alloc_retry=%llu\n",
  sc.name,sc.soft,accepted,froze,c.phase,(unsigned long long)s.encoded,(unsigned long long)s.diag.soft_cancel,
  (unsigned long long)s.diag.short_start,(unsigned long long)s.diag.geometry_skip,(unsigned long long)s.diag.alloc_retry);
 return accepted&&froze;
}
int main(int argc,char **argv){
 int v=argc>1;struct scen S[]={
  {"hw-like soft",1,5000,300000,6,800},
  {"hw-like old (soft off)",0,5000,300000,6,800},
  {"no pause soft",1,5000,0,0,0},
  {"long pause 1s soft",1,5000,1000000,10,1500},
  {"cancel before step soft",1,0,300000,6,800},
 };
 int ok=1;for(unsigned i=0;i<sizeof S/sizeof*S;i++){int r=run(S[i],v);if(S[i].soft)ok&=r;else ok&=!r;}
 printf(ok?"REPLAY PASS\n":"REPLAY FAIL\n");return !ok;
}
