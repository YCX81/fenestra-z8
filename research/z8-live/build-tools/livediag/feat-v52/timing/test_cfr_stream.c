#include "live_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct la_capture c;static struct live_stream s;
static uint8_t y[3][1280*852],uv[3][1280*852/2];
static uint64_t now,source_stamp,records[150000],source_records[150000];
static unsigned bank,valid_calls,fail_after_copy,cancel_copy,busy,allocations,frees,nrec,w=64,h=48;
static int alloc_(void*v,uint32_t n,void**d,uint8_t**p,uint32_t*a){(void)v;*d=*p=aligned_alloc(16,(n+15u)&~15u);*a=n;allocations++;return *p!=0;}
static void free_(void*v,void*d){(void)v;frees++;free(d);}
static int begin_(void*v,uint64_t g,uint64_t t,struct la_source*out){(void)v;valid_calls=0;*out=(struct la_source){y[bank],uv[bank],w,h,w,w,g,t,c.cancel_epoch};return 1;}
static int valid_(void*v,const struct la_source*x){(void)v;(void)x;valid_calls++;if(valid_calls==2&&cancel_copy){c.cancel_epoch++;return 0;}return !(valid_calls==2&&fail_after_copy);}
static void end_(void*v,const struct la_source*x){(void)v;(void)x;}
static uint64_t clk(void){return now;}
int live_encoder_sizes(unsigned w,unsigned h,size_t*p,size_t*t){(void)w;(void)h;*p=256;*t=256;return 0;}
int live_encoder_init(live_encoder*e,void*p,void*t,unsigned w,unsigned h){memset(e,0,sizeof(*e));e->persist=p;e->scratch=t;e->width=w;e->height=h;return 0;}
void live_encoder_set_flags(live_encoder*e,uint32_t f){(void)e;(void)f;}
int live_encoder_frame_timed(live_encoder*e,const uint8_t*in,uint64_t pts,uint64_t*last,unsigned b,unsigned interval,const uint8_t**out,size_t*bytes,unsigned*idr){
 (void)b;(void)interval;if(busy)return LIVE_ENCODER_BUSY;uint64_t src;memcpy(&src,in,8);
 assert(src<=pts);assert(src!=UINT64_C(0xffffeeee));assert(nrec<150000);records[nrec]=pts;source_records[nrec++]=src;
 static uint8_t au[32];memset(au,0,sizeof au);memcpy(au,&pts,8);memcpy(au+8,&e->frames,4); /* each invocation makes a new AU */
 *out=au;*bytes=sizeof au;*idr=e->frames%15u==0;if(*idr)*last=pts;e->last_pts=pts;e->frames++;return 0;
}
static void init(void){memset(&s,0,sizeof s);struct la_ops ops={&c,alloc_,free_,begin_,valid_,end_};la_init(&c,&ops);live_stream_init(&s);s.fr_rows=0;s.tune.cached_lv=s.tune.cached_work=s.tune.cached_slot=s.tune.cached_bs=0;la_request(&c,1);fail_after_copy=cancel_copy=busy=0;nrec=0;}
static void stop_(void){la_request(&c,0);s.job=0;live_stream_step(&c,&s,now,1,clk);assert(!c.descriptor&&!s.cfr_spare&&!s.cfr_have);if(s.fr_desc){free_(0,s.fr_desc);s.fr_desc=0;}}
static void signal_(uint64_t t,uint64_t value){source_stamp=t;bank=(bank+1)%3;memset(y[bank],50,sizeof y[bank]);memset(uv[bank],128,sizeof uv[bank]);memcpy(y[bank],&value,8);la_irq_signal(&c,0x41070030,1,t);}
static void tick_(uint64_t t){now=t;live_stream_step(&c,&s,t,1,clk);}
static void regular(unsigned ms,int press_ms,int gap1,int gap2){for(unsigned i=0;i<=ms;i++){uint64_t t=1000000u+(uint64_t)i*1000u;if(i%50==0&&(!(i>=gap1&&i<gap2)||gap1<0))signal_(t,t);if((int)i==press_ms)assert(la_full_press(&c,t));tick_(t);}}
int main(void){
 init();regular(5000,2500,2700,3150);assert(s.job==2&&s.frozen_count>85&&s.cfr_repeats>15);
 uint64_t epoch=s.cfr_epoch;for(unsigned i=0;i<nrec;i++)assert(records[i]==epoch+(uint64_t)i*1001000u/30u);
 assert(s.clip_end>=s.clip_press+1500000u&&s.clip_end<s.clip_press+1533367u);
 assert(s.last_duration_us==33366u||s.last_duration_us==33367u);assert(s.snapshot_last+s.last_duration_us==s.clip_end);
 assert(s.frozen_count*1001000u/30u+1u>=s.clip_end-s.clip_start&&s.frozen_count*1001000u/30u<=s.clip_end-s.clip_start+1u);
 printf("frozen=%u window=%llu last=%u repeats=%llu allocation=%u\n",s.frozen_count,(unsigned long long)(s.clip_end-s.clip_start),s.last_duration_us,(unsigned long long)s.cfr_repeats,s.allocation_requested);
 unsigned before=nrec;s.job=4;tick_(6100000u);assert(s.cfr_have==0&&s.ring.count==0&&nrec==before);signal_(6200000u,6200000u);tick_(6200000u);assert(s.cfr_epoch==6200000u&&s.cfr_index==1);stop_();
 // A rejected second lease check cannot overwrite the reusable last good image.
 init();signal_(1000000,1000000);tick_(1000000);fail_after_copy=1;signal_(1030000,0xffffeeee);tick_(1033366);assert(nrec==2&&source_records[1]==1000000);fail_after_copy=0;
 // Cancellation during copying prevents encoding; the following soft-cancel
 // step reuses the intact previous picture and retries the same missing tick.
 cancel_copy=1;signal_(1060000,0xffffeeee);tick_(1066733);assert(nrec==2);cancel_copy=0;tick_(1066800);assert(nrec==3&&source_records[2]==1000000);
 // Encoder busy does not consume or retime the presentation slot.
 busy=1;tick_(1100100);assert(nrec==3);busy=0;tick_(1133466);assert(nrec==4&&records[3]==1100100);tick_(1133467);assert(nrec==5&&records[4]==1133466);
 // No active clip: a long source interruption discards stale pre-roll.
 tick_(2100001);assert(!s.cfr_have&&!s.ring.count);signal_(2200000,2200000);tick_(2200000);assert(s.cfr_epoch==2200000&&s.ring.count==1);stop_();
 // Long-run drift test, using the real state machine but disabling only the
 // expensive diagnostic preview copy. No queued source and no frame retiming.
 init();signal_(1000000,1000000);tick_(1000000);s.preview=0;
 for(unsigned i=1;i<=108000;i++){uint64_t t=1000000u+(uint64_t)i*1001000u/30u;signal_(t,t);tick_(t);}
 assert(nrec==108001&&records[108000]==UINT64_C(3604600000));assert(s.effective_fps_milli==29970);stop_();
 // An encoder that stays busy past the post-roll grace fails instead of
 // producing a shortened clip whose timestamps falsely claim CFR.
 init();regular(2300,2000,-1,-1);busy=1;for(unsigned i=2301;i<=4600;i++){uint64_t t=1000000u+(uint64_t)i*1000u;if(i%50==0)signal_(t,t);tick_(t);if(s.error==LA_OVERFLOW)break;}assert(!s.job&&!s.cfr_have&&s.error==LA_OVERFLOW);stop_();
 // EVF geometry is scaled into 1024x682. Both I420 buffers are in the same
 // allocation, aligned, disjoint and clear of encoder scratch/work regions.
 init();w=1280;h=852;signal_(7000000,0);memset(y[bank],0,1280);tick_(7000000);assert(s.width==1024&&s.height==682);size_t frame=(size_t)s.width*s.height*3/2;
 uintptr_t a=(uintptr_t)s.i420,b=(uintptr_t)s.cfr_spare,base=(uintptr_t)c.pixels,end=base+c.actual_bytes;
 assert(!(a&15)&&!(b&15)&&a>=base&&b>=base&&a+frame<=end&&b+frame<=end&&(a+frame<=b||b+frame<=a));
 printf("evf requested=%u actual=%u i420=%zu spare=%zu frame=%zu stream_bytes=%zu\n",s.allocation_requested,c.actual_bytes,a-base,b-base,frame,sizeof s);stop_();
 assert(allocations==frees);puts("PASS CFR cadence, real source selection, repeats, freeze/audio window, stale gap, cancel, busy, drift, EVF allocation");
}
