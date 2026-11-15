#include "live_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *y,*uv;static unsigned w=1024,h=680,stride=1040,allocs,frees;static uint64_t fake_time;
static int alloc_(void *v,uint32_t n,void **d,uint8_t **p,uint32_t *a){(void)v;*d=*p=aligned_alloc(16,(n+15)&~15u);*a=n;allocs++;return *p!=0;}
static void free_(void *v,void *d){(void)v;free(d);frees++;}
static int begin_(void *v,uint64_t g,uint64_t t,struct la_source *s){(void)v;*s=(struct la_source){y,uv,w,h,stride,stride,g,t,0};return 1;}
static int valid_(void *v,const struct la_source *s){struct la_capture *c=v;return s->generation*2==c->irq_seq;}
static void end_(void *v,const struct la_source *s){(void)v;(void)s;}
static uint64_t clock_(void){return fake_time+=100;}
static int write_(void *v,uint64_t at,const void *p,size_t n){FILE *f=v;return fseek(f,at,SEEK_SET)||fwrite(p,1,n,f)!=n;}
static void step(struct la_capture *c,struct live_stream *s,uint64_t t,unsigned frame){
 for(unsigned row=0;row<h;row++)for(unsigned col=0;col<w;col++)y[(size_t)row*stride+col]=16+(col+row+frame*3)%220;
 for(unsigned row=0;row<h/2;row++)for(unsigned col=0;col<w/2;col++){uv[(size_t)row*stride+col*2]=90+frame%50;uv[(size_t)row*stride+col*2+1]=160-frame%50;}
 la_irq_signal(c,0x41070030,1,t);live_stream_step(c,s,t,1,clock_);
}
int main(int argc,char **argv){assert(argc==2);y=malloc(stride*h);uv=malloc(stride*h/2);assert(y&&uv);
 struct la_capture c;struct live_stream *s=calloc(1,sizeof(*s));assert(s);
 struct la_ops ops={&c,alloc_,free_,begin_,valid_,end_};la_init(&c,&ops);live_stream_init(s);la_request(&c,1);
 uint64_t press=0;
 for(unsigned i=0;i<150;i++){
  uint64_t t=1000000+(uint64_t)i*41667;
  if(i==65){assert(c.phase==LA_READY);press=t;assert(la_full_press(&c,press));}
  if(i==80){for(unsigned k=0;k<5;k++)la_irq_signal(&c,0x41070030,1,t-6+k);}
  step(&c,s,t,i);
  if(i==70)assert(!la_full_press(&c,t));
 }
 assert(s->job==2 && s->encoded==150 && s->backlog_dropped==5);
 assert(s->snapshot_first<=press-LA_WINDOW_US && press-LA_WINDOW_US-s->snapshot_first<=LIVE_IDR_US+41667);
 assert(s->frozen_count>60 && s->frozen_count<=LIVE_MAX_FRAMES && s->last_duration_us>0);
 assert(s->snapshot_last<press+LA_WINDOW_US && s->effective_fps_milli>=23990 && !s->low_fps);
 /* PREV.LVP source: 45 frames over the frozen window, from the 15 fps preview ring. */
 {static uint8_t f0[LIVE_PREVIEW_FRAME],fi[LIVE_PREVIEW_FRAME];unsigned differ=0;
  assert(s->preview_count>=45 && s->preview_count<=LIVE_PREVIEW_SLOTS);
  assert(!live_stream_preview_frame(s,0,f0,sizeof f0));
  for(unsigned i=1;i<45;i++){assert(!live_stream_preview_frame(s,i,fi,sizeof fi));differ+=memcmp(f0,fi,sizeof fi)!=0;}
  assert(live_stream_preview_frame(s,0,fi,sizeof fi-1)<0 && differ>=30);}
 size_t p,t;assert(!live_encoder_sizes(w,h,&p,&t));
 printf("native=%ux%u persist=%zu scratch=%zu allocation=%u actual=%u frozen_frames=%u frozen_bytes=%u fps_milli=%llu dropped=%llu\n",w,h,p,t,s->allocation_requested,c.actual_bytes,s->frozen_count,s->frozen_size,(unsigned long long)s->effective_fps_milli,(unsigned long long)c.dropped);
 /* Deliberately evict ALL rolling bytes while the frozen consumer is live. */
 uint8_t *saved=malloc(s->frozen_size),*junk=malloc(LIVE_BITSTREAM_BYTES);assert(saved&&junk);memcpy(saved,s->frozen_bytes,s->frozen_size);memset(junk,0xcc,LIVE_BITSTREAM_BYTES);
 assert(!au_ring_push(&s->ring,junk,LIVE_BITSTREAM_BYTES,9000000,1));assert(s->ring.count==1);
 assert(!memcmp(saved,s->frozen_bytes,s->frozen_size));
 FILE *f=fopen(argv[1],"w+b");assert(f);live_mov *m=calloc(1,sizeof(*m));assert(m);assert(!live_mov_begin(m,write_,f,w,h));
 for(unsigned i=0;i<s->frozen_count;i++)assert(!live_mov_add(m,s->frozen[i].annexb,s->frozen[i].bytes,s->frozen[i].pts_us));
 assert(!live_mov_finish(m,press,s->last_duration_us,"4C495645-5048-4F54-8F41-303030303031"));assert(!fclose(f));
 /* Cancellation cannot free the arena while mux owns it. */
 s->job=3;la_request(&c,0);live_stream_step(&c,s,10000000,1,clock_);assert(c.descriptor&&frees==0);
 assert(!memcmp(saved,s->frozen_bytes,s->frozen_size));s->job=4;live_stream_step(&c,s,10000001,1,clock_);assert(!c.descriptor&&frees==1);
 /* Geometry is read per session, with no fixed compile-time dimensions. */
 w=640;h=424;la_request(&c,1);step(&c,s,11000000,151);assert(s->width==640&&s->height==424&&allocs==2);
 w=1024;h=680;step(&c,s,11041667,152);assert(s->geometry_changes==1);live_stream_step(&c,s,11041668,1,clock_);assert(frees==2);
 la_request(&c,0);live_stream_step(&c,s,11041669,1,clock_);
 /* Sustained five fps must remain timestamped and set the diagnostic flag. */
 la_request(&c,1);
 for(unsigned i=0;i<21;i++)step(&c,s,12000000+(uint64_t)i*200000,200+i);
 assert(s->low_fps && s->effective_fps_milli==5000);
 assert(s->encoded==172 && s->ring.count==21);
 la_request(&c,0);live_stream_step(&c,s,17000000,1,clock_);assert(frees==3);

 free(y);free(uv);free(saved);free(junk);free(m);free(s);
 puts("PASS: full B1, runtime geometry, timestamp IDR, backlog drop, independent frozen bytes, cancellation ACK, mux after eviction, sustained <15 fps diagnostic");return 0;
}
