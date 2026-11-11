#include "live_stream.h"
#include "live_alias.h"
#include <string.h>
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
static uintptr_t align16(uintptr_t a){return (a+15)&~(uintptr_t)15;}
void live_stream_init(struct live_stream *s) {
 memset(s,0,sizeof(*s));s->fr_magic=0x31524646u;s->fr_rows=16;s->fr_drop=1;s->dcim_copy=0 /* v37: one MOV per shot */;s->bitrate=LIVE_BITRATE;s->idr_interval_us=LIVE_IDR_US;
 s->tune=(struct live_tune){0x314e544cu,1,-1,1,LA_WINDOW_US,29000u /* v20 */,LA_MAX_GAP_US,1,3000000u,1,1000000u,1,1,1,1,3,400u /* v37 */};
}
/* Source rows sit in an uncached live-view buffer: one ldp moves 16 bytes, so a 64-byte
 * burst costs 4 bus reads instead of 64 byte loads. Falls back to bytes when unaligned. */
static void burst_copy(uint8_t *d,const uint8_t *s,size_t n) {
 if((((uintptr_t)d|(uintptr_t)s)&15)==0){
  while(n>=64){
   __asm__ volatile("ldp x9,x10,[%1]\n ldp x11,x12,[%1,#16]\n ldp x13,x14,[%1,#32]\n ldp x15,x16,[%1,#48]\n"
                    "stp x9,x10,[%0]\n stp x11,x12,[%0,#16]\n stp x13,x14,[%0,#32]\n stp x15,x16,[%0,#48]\n"
                    ::"r"(d),"r"(s):"x9","x10","x11","x12","x13","x14","x15","x16","memory");
   d+=64;s+=64;n-=64;}
 }
 while(n--)*d++=*s++;
}
int live_stream_convert(uint8_t *dst,const struct la_source *s) {
 if(!dst||!s->y||!s->uv||!s->width||!s->height||((s->width|s->height)&1)||
 s->width>4096||s->height>4096||s->y_stride<s->width||s->uv_stride<s->width||
 s->y_stride>8192||s->uv_stride>8192)return 0;
 unsigned w=s->width,h=s->height;uint8_t *u=dst+(size_t)w*h,*v=u+(size_t)w*h/4;
 uint8_t tmp[4096] __attribute__((aligned(16)));
 for(unsigned y=0;y<h;y++)burst_copy(dst+(size_t)y*w,s->y+(size_t)y*s->y_stride,w);
 for(unsigned y=0;y<h/2;y++){burst_copy(tmp,s->uv+(size_t)y*s->uv_stride,w);
  for(unsigned x=0;x<w/2;x++){u[(size_t)y*w/2+x]=tmp[2*x];v[(size_t)y*w/2+x]=tmp[2*x+1];}}
 return 1;
}
static void release(struct la_capture *c,struct live_stream *s) {
 if(s->cache_base){la_dc_civac(s->cache_base,s->cache_size);s->cache_base=0;s->cache_size=0;}
 if(c->descriptor)c->ops.release(c->ops.context,c->descriptor);
 c->descriptor=0;c->pixels=0;c->actual_bytes=0;c->count=0;c->stream_start=0;
 s->i420=0;s->work=0;s->frozen_bytes=0;s->preview=0;s->preview_scratch=0;s->preview_head=0;s->preview_count=0;s->preview_last=0;s->last_pts=0;s->last_idr_us=0;
 au_ring_init(&s->ring,0,0);STORE(&c->press_mailbox,0);STORE(&s->job,0);STORE(&c->phase,LA_OFF);
}
static int allocate(struct la_capture *c,struct live_stream *s,unsigned w,unsigned h) {
 if(!s->fr && !s->fr_desc){void *d=0;uint8_t *px=0;uint32_t got=0;
  if(c->ops.allocate(c->ops.context,LIVE_FR*(uint32_t)sizeof(struct live_fr)+64u,&d,&px,&got) && d && px &&
     got>=LIVE_FR*sizeof(struct live_fr)+64u){s->fr_desc=d;s->fr=(struct live_fr *)(((uintptr_t)px+63)&~(uintptr_t)63);
   memset(s->fr,0,LIVE_FR*sizeof(struct live_fr));s->fr_head=0;}
  else{s->fr_alloc_fail++;if(d)c->ops.release(c->ops.context,d);}}
 size_t p,t;if(live_encoder_sizes(w,h,&p,&t))return 0;
 size_t bytes=2*LIVE_BITSTREAM_BYTES+(size_t)w*h*3/2+p+t+sizeof(*s->work)+(LIVE_PREVIEW_SLOTS+1)*(size_t)LIVE_PREVIEW_FRAME+160;
 if(bytes>UINT32_MAX)return 0;s->allocation_requested=bytes;
 if(!c->ops.allocate(c->ops.context,bytes,&c->descriptor,&c->pixels,&c->actual_bytes))return 0;
 if(!c->descriptor||!c->pixels||c->actual_bytes<bytes){release(c,s);return 0;}
 if(s->tune.cached_work && !(((uintptr_t)c->pixels|c->actual_bytes)&63)){
  uintptr_t a=la_alias((uintptr_t)c->pixels,c->actual_bytes);
  if(a){la_dc_civac(a,c->actual_bytes);c->pixels=(uint8_t *)a;s->cache_base=a;s->cache_size=c->actual_bytes;s->cached_work_ok++;}
  else s->cached_work_fail++;
 }
 uintptr_t a=align16((uintptr_t)c->pixels);s->i420=(void *)a;a=align16(a+(size_t)w*h*3/2);
 void *pm=(void *)a;a=align16(a+p);void *tm=(void *)a;a=align16(a+t);
 s->work=(void *)a;a=align16(a+sizeof(*s->work));
 au_ring_init(&s->ring,(void *)a,LIVE_BITSTREAM_BYTES);a+=LIVE_BITSTREAM_BYTES;s->frozen_bytes=(void *)a;
 a=align16(a+LIVE_BITSTREAM_BYTES);s->preview=(void *)a;a=align16(a+LIVE_PREVIEW_SLOTS*(size_t)LIVE_PREVIEW_FRAME);s->preview_scratch=(void *)a;
 s->preview_head=0;s->preview_count=0;s->preview_last=0;
 s->width=w;s->height=h;s->first_pts=0;s->last_pts=0;
 if(live_encoder_init(&s->encoder,pm,tm,w,h)){release(c,s);return 0;}return 1;
}
/* Nearest-neighbour I420 downscale of the frame just encoded into one slot. */
static void preview_push(struct live_stream *s,uint64_t pts) {
 if(!s->preview||(s->preview_last&&pts-s->preview_last<LIVE_PREVIEW_STEP_US-8000u))return;
 unsigned w=s->width,h=s->height;const uint8_t *sy=s->i420,*su=sy+(size_t)w*h,*sv=su+(size_t)w*h/4;
 uint8_t *d=s->preview+(size_t)s->preview_head*LIVE_PREVIEW_FRAME,*du=d+LIVE_PREVIEW_W*LIVE_PREVIEW_H,*dv=du+LIVE_PREVIEW_W*LIVE_PREVIEW_H/4;
 for(unsigned y=0;y<LIVE_PREVIEW_H;y++){const uint8_t *row=sy+(size_t)(y*h/LIVE_PREVIEW_H)*w;
  for(unsigned x=0;x<LIVE_PREVIEW_W;x++)d[y*LIVE_PREVIEW_W+x]=row[x*w/LIVE_PREVIEW_W];}
 for(unsigned y=0;y<LIVE_PREVIEW_H/2;y++){size_t r=(size_t)(y*(h/2)/(LIVE_PREVIEW_H/2))*(w/2);
  for(unsigned x=0;x<LIVE_PREVIEW_W/2;x++){size_t c=r+x*(w/2)/(LIVE_PREVIEW_W/2);
   du[y*(LIVE_PREVIEW_W/2)+x]=su[c];dv[y*(LIVE_PREVIEW_W/2)+x]=sv[c];}}
 s->preview_pts[s->preview_head]=pts;s->preview_head=(s->preview_head+1)%LIVE_PREVIEW_SLOTS;
 if(s->preview_count<LIVE_PREVIEW_SLOTS)s->preview_count++;
 s->preview_last=pts;
}
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
static void freeze(struct la_capture *c,struct live_stream *s) {
 unsigned start;if(au_ring_find_start(&s->ring,s->clip_start,&start))goto fail;
 if(s->ring.entries[(s->ring.head+start)%AU_RING_SLOTS].pts!=s->clip_start)goto fail;
 unsigned count=0,bytes=0;
 for(unsigned i=start;i<s->ring.count;i++){
  const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
  if(e->pts>=s->clip_end)break;
  if(count==LIVE_MAX_FRAMES || e->size>LIVE_BITSTREAM_BYTES-bytes)goto fail;
  if(au_ring_copy(&s->ring,i,s->frozen_bytes+bytes,LIVE_BITSTREAM_BYTES-bytes)<0)goto fail;
  s->frozen[count++]=(live_input_au){s->frozen_bytes+bytes,e->size,e->pts};bytes+=e->size;
 }
 if(!count||s->frozen[count-1].pts_us<s->clip_press)goto fail;
 s->frozen_count=count;s->frozen_size=bytes;s->last_duration_us=s->clip_end-s->frozen[count-1].pts_us;
 s->snapshot_first=s->frozen[0].pts_us;s->snapshot_last=s->frozen[count-1].pts_us;
 s->snapshot_encoded=s->encoded;STORE(&c->phase,LA_FROZEN);STORE(&s->job,2);return;
 fail:s->error=LA_OVERFLOW;STORE(&s->job,0);STORE(&c->phase,LA_REQUESTED);
}
static int fr_row_diff(const uint8_t *a,const uint8_t *b,unsigned n){
 if(!(((uintptr_t)a|(uintptr_t)b)&7)){const uint64_t *x=(const uint64_t *)a,*y=(const uint64_t *)b;
  for(unsigned i=0;i<n/8;i++)if(x[i]!=y[i])return 1;return 0;}
 for(unsigned i=0;i<n;i++)if(a[i]!=b[i])return 1;return 0;}
/* Re-read sample rows of the source right after the copy; compare with what was copied. */
static void fr_sample(struct live_stream *s,const struct la_source *src,const struct la_source *rd,struct live_fr *f){
 unsigned R=s->fr_rows>16?16:s->fr_rows,w=src->width,h=src->height;
 const uint8_t *u=s->i420+(size_t)w*h,*v=u+(size_t)w*h/4;
 for(unsigned k=0;k<R;k++){unsigned yy=(unsigned)(((uint64_t)k*2+1)*h/(2*R));
  const uint8_t *sr=rd->y+(size_t)yy*src->y_stride;
  if(rd->y!=src->y)la_dc_ivac((uintptr_t)sr,w);
  if(fr_row_diff(sr,s->i420+(size_t)yy*w,w))f->torn_y|=(uint16_t)(1u<<k);
  unsigned uy=yy/2;const uint8_t *su=rd->uv+(size_t)uy*src->uv_stride;
  if(rd->uv!=src->uv)la_dc_ivac((uintptr_t)su,w);
  for(unsigned x=0;x<w/2;x++)if(su[2*x]!=u[(size_t)uy*w/2+x]||su[2*x+1]!=v[(size_t)uy*w/2+x]){f->torn_uv|=(uint16_t)(1u<<k);break;}}
 s->fr_checked++;if(f->torn_y|f->torn_uv)s->fr_torn++;}
static void fr_push(struct live_stream *s,struct live_fr *f,const struct live_diag *d0){
 const struct live_diag *D=&s->diag;uint8_t r=0;
 if(f->flags&4)r=15;else if(f->flags&1)r=14;else if(D->busy>d0->busy)r=13;else if(D->ok>d0->ok)r=D->accepted_late>d0->accepted_late?2:1;
 else if(D->stale>d0->stale)r=3;else if(D->interval>d0->interval)r=4;else if(D->lease_fail>d0->lease_fail)r=5;
 else if(D->alloc_retry>d0->alloc_retry)r=6;else if(D->geometry>d0->geometry)r=7;else if(D->valid1_fail>d0->valid1_fail)r=8;
 else if(D->convert_fail>d0->convert_fail)r=9;else if(D->cancelled>d0->cancelled)r=10;else if(D->seq_changed>d0->seq_changed)r=11;
 else if(D->valid2_fail>d0->valid2_fail)r=12;
 f->result=r;f->nbufs=(uint8_t)D->nbufs;
 if(!s->fr)return;uint32_t i=s->fr_head%LIVE_FR;s->fr[i]=*f;__atomic_store_n(&s->fr_head,s->fr_head+1,__ATOMIC_RELEASE);}
void live_stream_step(struct la_capture *c,struct live_stream *s,uint64_t now,int eligible,uint64_t (*clock_us)(void)) {
 struct live_fr fr;int fr_on=0;struct live_diag d0;
 uint32_t epoch=LOAD(&c->cancel_epoch),job=LOAD(&s->job);
 s->diag.magic=0x3147444cu;s->diag.version=9;s->diag.steps++;if(!eligible)s->diag.ineligible++;
 if(eligible && LOAD(&c->requested) && epoch!=c->worker_epoch && s->tune.soft_cancel) {
  /* C0 start/stop/hold and the still pool cancel at every shutter: the live-view source is
   * void, but encoded AUs are our own memory. Drop only the frame in flight. */
  c->worker_epoch=epoch;s->diag.soft_cancel++;live_log(s,now,EV_SOFT_CANCEL,epoch,job);
 }
 if(!eligible || !LOAD(&c->requested) || epoch!=c->worker_epoch) {
  if(!eligible && epoch==c->worker_epoch)epoch=__atomic_add_fetch(&c->cancel_epoch,1,__ATOMIC_ACQ_REL);
  /* Mux reads frozen storage: cancellation first, release after its ACK. */
  if(job==2||job==3)return;
  if(c->descriptor||job)live_log(s,now,EV_RELEASE,eligible|LOAD(&c->requested)<<1,job);
  release(c,s);job=0;c->worker_epoch=epoch;
  if(!eligible||!LOAD(&c->requested))return;
 }
 if(job==4){STORE(&s->job,0);job=0;STORE(&c->phase,LA_REQUESTED);}
 if(c->phase==LA_FAILED)return;
 uint64_t press=__atomic_exchange_n(&c->press_mailbox,0,__ATOMIC_ACQ_REL);
 if(press){
  unsigned start;
  uint32_t W=s->tune.window_us;
  int found=!job && press>=W && !au_ring_find_start(&s->ring,press-W,&start);
  if(found)live_log(s,now,EV_ACCEPT,start,press);
  if(!job && !found && s->tune.short_start){
   /* No IDR 1.5 s back (slow encode or a young ring): start at the earliest IDR before the press. */
   for(unsigned i=0;i<s->ring.count;i++){const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
    if(e->pts>press)break;if(e->idr){start=i;found=1;s->diag.short_start++;live_log(s,now,EV_SHORT,i,press);break;}}
  }
  if(found){
   s->clip_start=s->ring.entries[(s->ring.head+start)%AU_RING_SLOTS].pts;
   s->clip_press=press;s->clip_end=press+W;c->press_us=press;c->accepted_presses++;
   job=1;STORE(&s->job,1);STORE(&c->phase,LA_POST);
  }else{s->ignored_presses++;c->rejected_presses++;live_log(s,now,EV_IGNORED,s->ring.count|job<<16,press);}
 }
 uint64_t seq=LOAD(&c->irq_seq),stamp=LOAD(&c->irq_timestamp);
 if((seq&1)||seq!=LOAD(&c->irq_seq))return;
 if(seq && seq!=c->last_generation && stamp<=now) {
  if(c->last_generation && seq>c->last_generation+2){uint64_t n=(seq-c->last_generation)/2-1;c->dropped+=n;s->backlog_dropped+=n;}
  c->last_generation=seq;
  struct live_diag *D=&s->diag;D->new_seq++;
  fr_on=1;d0=*D;memset(&fr,0,sizeof fr);fr.t=now;fr.seq=seq;fr.picker=(uint8_t)s->tune.picker;
  D->last_age_us=now-stamp;if(D->last_age_us>D->max_age_us)D->max_age_us=D->last_age_us;
  if(now-stamp>s->tune.stale_us){D->stale++;c->dropped++;s->copy_dropped++;goto finish;}
  struct la_source source={0};
  uint64_t t0=clock_us();
  int began=c->ops.lease_begin(c->ops.context,seq/2,stamp,&source);
  uint64_t t1=clock_us();D->last_lease_us=t1-t0;if(D->last_lease_us>D->max_lease_us)D->max_lease_us=D->last_lease_us;
  if(!began){D->lease_fail++;c->dropped++;s->copy_dropped++;goto finish;}
  D->y=(uint64_t)(uintptr_t)source.y;D->uv=(uint64_t)(uintptr_t)source.uv;D->width=source.width;D->height=source.height;
  D->y_stride=source.y_stride;D->uv_stride=source.uv_stride;D->version2=2;
  fr.y=D->y;{uint32_t sh=s->snap_head;if(sh){fr.s4=s->snap[(sh-1)%16u].s4;fr.s20=s->snap[(sh-1)%16u].s20;}}
  if(s->last_kept_y&&D->y==s->last_kept_y){
   c->ops.lease_end(c->ops.context,&source);fr.flags|=4;
   /* v18: the current-buffer word may move a few ms after the interrupt: try this frame again */
   uint32_t key=(uint32_t)(seq<<4),tries=(s->preview_pad&~15u)==key?(s->preview_pad&15u):0u;
   if(tries<3u){s->preview_pad=key|(tries+1u);c->last_generation=seq-2u;goto finish;}
   s->dup_skipped++;c->dropped++;goto finish;}
  /* v13: the interval gate applies only to a new buffer (after the duplicate check). */
  if(stamp<=s->last_pts || (s->last_pts && stamp-s->last_pts<s->tune.sample_us)){c->ops.lease_end(c->ops.context,&source);D->interval++;c->dropped++;goto finish;}
  {unsigned k;for(k=0;k<D->nbufs && D->bufs[k]!=D->y;k++){}
   if(k==D->nbufs && D->nbufs<8)D->bufs[D->nbufs++]=D->y;}
  if(!c->descriptor && !allocate(c,s,source.width,source.height)){
   /* The native pool is gated while a still is processed: wait and retry on a later frame. */
   c->ops.lease_end(c->ops.context,&source);c->reason=LA_ALLOC;D->alloc_retry++;if(D->alloc_retry<64)live_log(s,now,EV_ALLOC,0,0);c->dropped++;goto finish;}
  if(source.width!=s->width||source.height!=s->height){
   /* Live view changes geometry around a still capture: skip those frames, keep the clip. */
   D->geometry++;D->geometry_skip++;s->geometry_changes++;c->ops.lease_end(c->ops.context,&source);c->dropped++;
   /* A lasting change (not around a capture, no clip in progress) restarts Live as before. */
   if(!D->geometry_since||job)live_log(s,now,EV_GEOMETRY,source.width<<16|source.height,job);
   if(!job){if(!D->geometry_since)D->geometry_since=now?now:1;
    else if(now-D->geometry_since>s->tune.geometry_reset_us){D->geometry_since=0;la_cancel(c);return;}}
   goto finish;}
  D->geometry_since=0;
  int valid=c->ops.lease_valid(c->ops.context,&source);
  if(!valid)D->valid1_fail++;
  else{
   struct la_source src2=source;
   if(s->tune.cached_lv){
    uintptr_t ya=la_alias((uintptr_t)source.y,(size_t)source.y_stride*source.height),
              ua=la_alias((uintptr_t)source.uv,(size_t)source.uv_stride*(source.height/2));
    if(ya&&ua){la_dc_ivac(ya,(size_t)source.y_stride*source.height);la_dc_ivac(ua,(size_t)source.uv_stride*(source.height/2));
     src2.y=(const uint8_t *)ya;src2.uv=(const uint8_t *)ua;s->cached_lv_ok++;}
    else s->cached_lv_fail++;
   }
   uint64_t t2=clock_us();valid=live_stream_convert(s->i420,&src2);uint64_t t3=clock_us();
   D->last_convert_us=t3-t2;if(D->last_convert_us>D->max_convert_us)D->max_convert_us=D->last_convert_us;
   D->sum_convert_us+=t3-t2;D->n_convert++;
   uint64_t after=LOAD(&c->irq_seq);D->last_frames_during_copy=after>seq?(after-seq)/2:0;
   fr.after=after;fr.conv_us=(uint32_t)(t3-t2);if(valid&&s->fr_rows&&s->fr)fr_sample(s,&source,&src2,&fr);
   if(D->last_frames_during_copy>D->max_frames_during_copy)D->max_frames_during_copy=D->last_frames_during_copy;
   if(!valid)D->convert_fail++;
   else if(la_cancelled(c)){valid=0;D->cancelled++;}
   else{
    uint64_t now_seq=LOAD(&c->irq_seq);uint64_t passed=now_seq>seq?(now_seq-seq+1)/2:0;
    int same=c->ops.lease_valid(c->ops.context,&source) && now_seq==seq;
    /* With N>=3 rotating buffers, the one just copied is rewritten only after N-1 newer
     * frames; accept when at most N-2 completed during the copy. */
    int64_t lim=s->tune.late_max<0?(D->nbufs>=3?(int64_t)D->nbufs-2:0):(s->tune.late_max>8?8:s->tune.late_max);
    int late=!same && !(now_seq&1) && lim>0 && passed<=(uint64_t)lim &&
             LOAD(&c->cancel_epoch)==source.lease;
    if(same){}
    else if(late){D->accepted_late++;}
    else if(now_seq!=seq && s->tune.picker>=2 && s->fr && s->fr_rows && !(fr.torn_y|fr.torn_uv) &&
            LOAD(&c->cancel_epoch)==source.lease){D->accepted_late++;fr.flags|=2;}
    else if(now_seq!=seq){valid=0;D->seq_changed++;}
    else{valid=0;D->valid2_fail++;}
   }
  }
  if(valid && s->fr_drop && (fr.torn_y|fr.torn_uv)){valid=0;s->fr_dropped++;fr.flags|=1;}
  c->ops.lease_end(c->ops.context,&source);
  if(!valid){c->dropped++;s->copy_dropped++;goto finish;}
  D->ok++;
  live_encoder_set_flags(&s->encoder,(s->tune.cached_slot?2u:0u)|(s->tune.cached_bs?4u:0u));
  c->copied++;uint64_t begin=clock_us();const uint8_t *au;size_t n;unsigned idr;
  int rc=live_encoder_frame_timed(&s->encoder,s->i420,stamp,&s->last_idr_us,s->bitrate,s->idr_interval_us,&au,&n,&idr);
  uint64_t elapsed=clock_us()-begin;s->encode_last_us=elapsed;fr.enc_us=(uint32_t)elapsed;
  if(rc==LIVE_ENCODER_BUSY){s->diag.busy++;c->dropped++;s->copy_dropped++;goto finish;}
 if(rc){s->diag.enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);s->error=-rc;STORE(&c->phase,LA_FAILED);return;}
  s->last_kept_y=D->y;s->encoded++;s->encode_total_us+=elapsed;if(elapsed>s->encode_max_us)s->encode_max_us=elapsed;
  s->encode_avg_us=s->encode_total_us/s->encoded;
  if(n>UINT32_MAX||au_ring_push(&s->ring,au,n,stamp,idr)){s->error=LA_OVERFLOW;STORE(&c->phase,LA_FAILED);return;}
  s->last_pts=stamp;if(!s->first_pts)s->first_pts=stamp;
  if(job<=1)preview_push(s,stamp);
  s->ring_fill=s->ring.used;if(s->ring_fill>s->ring_peak)s->ring_peak=s->ring_fill;c->count=s->ring.count;
  if(stamp>s->first_pts)s->effective_fps_milli=(uint64_t)(s->encoder.frames-1)*1000000000/(stamp-s->first_pts);
  s->low_fps=stamp-s->first_pts>=2500000 && s->effective_fps_milli<15000;
 }
 finish:
 if(fr_on)fr_push(s,&fr,&d0);
 if(job==1 && now>=s->clip_end){freeze(c,s);live_log(s,now,LOAD(&s->job)==2?EV_FREEZE:EV_FREEZE_FAIL,s->frozen_count,s->frozen_size|(uint64_t)s->error<<32);}
 else if(!job){unsigned start;
  if(now>=s->tune.window_us && s->last_pts && now-s->last_pts<=s->tune.ready_fresh_us &&
    !au_ring_find_start(&s->ring,now-s->tune.window_us,&start)){if(c->phase!=LA_READY)live_log(s,now,EV_READY,s->ring.count,0);STORE(&c->phase,LA_READY);}
  else STORE(&c->phase,LA_REQUESTED);
 }
}
