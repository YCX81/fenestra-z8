#include "capture/live_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *Y[3],*UV[3],*y,*uv;static unsigned w=1024,h=680,stride=1040;static uint64_t fake;
static int alloc_(void *v,uint32_t n,void **d,uint8_t **p,uint32_t *a){(void)v;*d=*p=aligned_alloc(64,(n+63)&~63u);*a=n;return *p!=0;}
static void free_(void *v,void *d){(void)v;free(d);}
static int begin_(void *v,uint64_t g,uint64_t t,struct la_source *s){(void)v;*s=(struct la_source){y,uv,w,h,stride,stride,g,t,0};return 1;}
static int valid_(void *v,const struct la_source *s){struct la_capture *c=v;return s->generation*2==c->irq_seq;}
static void end_(void *v,const struct la_source *s){(void)v;(void)s;}
static uint64_t clock_(void){return fake+=100;}
static unsigned fr;
static void step(struct la_capture *c,struct live_stream *s,uint64_t t){
 fr++;y=Y[fr%3];uv=UV[fr%3];memset(y,(int)(16+fr%200),(size_t)stride*h);memset(uv,128,(size_t)stride*h/2);
 la_irq_signal(c,0x41070030,1,t);live_stream_step(c,s,t,1,clock_);}
int main(void){setvbuf(stdout,0,_IONBF,0);for(int k=0;k<3;k++){Y[k]=malloc(stride*h);UV[k]=malloc(stride*h/2);}y=Y[0];uv=UV[0];
 struct la_capture c;struct live_stream *s=calloc(1,sizeof *s);
 struct la_ops ops={&c,alloc_,free_,begin_,valid_,end_};la_init(&c,&ops);live_stream_init(s);la_request(&c,1);
 uint64_t t=1000000,dt=33367;unsigned i;uint32_t warm_at=0,ready_at=0;
 for(i=0;i<80;i++){t+=dt;step(&c,s,t);uint32_t p=live_stream_progress(&c,s,t);
  if(c.phase==LA_WARM&&!warm_at)warm_at=i;if(c.phase==LA_READY&&!ready_at)ready_at=i;
  if(i%6==0)printf("start i=%u phase=%d prog=%u ring=%u\n",i,c.phase,p,s->ring.count);}
 printf("first: warm at frame %u (%.2fs) ready at %u (%.2fs)\n",warm_at,warm_at*dt/1e6,ready_at,ready_at*dt/1e6);
 assert(c.phase==LA_READY && warm_at && warm_at<ready_at);
 void *desc=c.descriptor;unsigned enc=s->encoded;
 /* menu: no frames for 3 s */
 t+=3000000;step(&c,s,t);
 printf("after gap: phase=%d ring=%u prog=%u same_mem=%d\n",c.phase,s->ring.count,live_stream_progress(&c,s,t),c.descriptor==desc);
 assert(c.descriptor==desc && s->ring.count==1 && c.phase==LA_REQUESTED && s->ring.entries[s->ring.head].idr);
 assert(s->ring.entries[s->ring.head].pts==t);
 /* press right after resume: ignored */
 assert(!la_full_press(&c,t));
 warm_at=ready_at=0;uint64_t t0=t;
 for(i=0;i<60;i++){t+=dt;step(&c,s,t);if(c.phase==LA_WARM&&!warm_at)warm_at=i+1;if(c.phase==LA_READY&&!ready_at)ready_at=i+1;}
 printf("resume: warm after %.2fs ready after %.2fs\n",warm_at*dt/1e6,ready_at*dt/1e6);
 assert(ready_at*dt>=1500000-dt && ready_at*dt<=1500000+2*dt && warm_at*dt>=500000-dt && warm_at*dt<=500000+2*dt);
 /* second gap, then press while WARM -> short clip from the resume IDR */
 t+=2000000;step(&c,s,t);t0=t;
 while(c.phase!=LA_WARM){t+=dt;step(&c,s,t);}
 uint64_t press=t;assert(la_full_press(&c,press));
 for(i=0;i<80 && s->job<2;i++){t+=dt;step(&c,s,t);}
 printf("warm press: accepted=%llu short=%llu clip_start-resume=%lld us job=%u frozen=%u enc=%llu\n",(unsigned long long)c.accepted_presses,(unsigned long long)s->diag.short_start,(long long)(s->clip_start-t0),s->job,s->frozen_count,(unsigned long long)(s->encoded-enc));
 assert(c.accepted_presses==1 && s->diag.short_start==1 && s->clip_start==t0 && s->job==2 && s->frozen_count>40);
 puts("PASS");}
