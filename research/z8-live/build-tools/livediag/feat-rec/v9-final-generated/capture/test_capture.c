#include "live_capture.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct fixture { unsigned allocations,frees,begins,ends,checks; int valid,fail_alloc; uint8_t *y,*uv; };
static int allocate(void *v,uint32_t n,void **d,uint8_t **p,uint32_t *size) {
    struct fixture *f=v;f->allocations++;
    if(f->fail_alloc)return 0;
    *d=*p=malloc(n);*size=n;return *d!=NULL;
}
static void release(void *v,void *d){((struct fixture *)v)->frees++;free(d);}
static int begin(void *v,uint64_t gen,uint64_t ts,struct la_source *s) {
    struct fixture *f=v;f->begins++;
    if(!f->valid)return 0;
    *s=(struct la_source){f->y,f->uv,1024,680,1088,1088,gen,ts,gen};return 1;
}
static int valid(void *v,const struct la_source *s) {
    struct fixture *f=v;(void)s;f->checks++;
    return f->valid!=2 || (f->checks&1); /* revoke exactly after conversion */
}
static void end(void *v,const struct la_source *s){(void)s;((struct fixture *)v)->ends++;}
static void tick(struct la_capture *c,uint64_t t){la_irq_signal(c,0x41070030,1,t);la_step(c,t,1);}
static void setup(struct la_capture *c,struct fixture *f) {
    memset(f,0,sizeof(*f));f->valid=1;
    f->y=malloc(1088*680);f->uv=malloc(1088*340);
    memset(f->y,37,1088*680);memset(f->uv,0,1088*340);
    for(int y=0;y<340;y++)for(int x=0;x<1024;x+=2){f->uv[y*1088+x]=79;f->uv[y*1088+x+1]=193;}
    struct la_ops ops={f,allocate,release,begin,valid,end};la_init(c,&ops);la_request(c,1);
}
static void teardown(struct la_capture *c,struct fixture *f) {
    la_request(c,0);la_step(c,10000000,1);assert(f->allocations==f->frees || f->fail_alloc);
    free(f->y);free(f->uv);
}
int main(void) {
    struct la_capture c;struct fixture f;
    setup(&c,&f);f.valid=0;tick(&c,100000);assert(f.allocations==1 && c.reason==LA_SOURCE_UNPROVEN);
    teardown(&c,&f);
    setup(&c,&f);f.fail_alloc=1;tick(&c,100000);assert(c.phase==LA_FAILED && !c.pixels);teardown(&c,&f);
    setup(&c,&f);f.valid=2;tick(&c,100000);assert(!c.count && c.dropped==1 && f.ends==1);teardown(&c,&f);
    setup(&c,&f);
    uint64_t t=100000;
    for(unsigned i=0;i<48;i++,t+=41667)tick(&c,t);
    assert(c.phase==LA_READY && c.count>=35 && c.count<=45);
    assert(la_full_press(&c,t));assert(!la_full_press(&c,t+1));
    tick(&c,t);assert(c.phase==LA_POST);assert(!la_full_press(&c,t+100));
    uint64_t press=t;
    for(unsigned i=1;i<=37;i++)tick(&c,press+i*41667);
    assert(c.phase==LA_FROZEN && c.count<=LA_RING_CAPACITY && c.press_us==press);
    assert(la_begin_encode(&c));
    struct la_frame meta;const uint8_t *p=la_frame_at(&c,0,&meta);
    assert(p && p[0]==37 && p[LA_WIDTH*LA_HEIGHT]==79 && p[LA_WIDTH*LA_HEIGHT*5/4]==193);
    assert(meta.timestamp_us<press);
    la_cancel(&c);la_step(&c,press+2000000,1);assert(!f.frees);assert(!la_frame_at(&c,0,0));
    la_finish_encode(&c);assert(f.frees==1 && !c.pixels);teardown(&c,&f);
    setup(&c,&f);t=100000;
    for(unsigned i=0;i<48;i++,t+=41667)tick(&c,t);
    la_step(&c,t+200000,1);assert(c.phase==LA_REQUESTED && !c.count);teardown(&c,&f);
    setup(&c,&f);t=100000;
    for(unsigned i=0;i<60;i++,t+=33334)tick(&c,t);
    assert(c.phase==LA_READY);assert(la_full_press(&c,t));press=t;
    for(unsigned i=0;i<=46;i++)tick(&c,press+i*33334);
    assert(c.phase==LA_FROZEN && c.count<=LA_RING_CAPACITY);assert(la_begin_encode(&c));
    la_step(&c,press+2000000,0);assert(la_cancelled(&c) && !f.frees);
    la_finish_encode(&c);assert(f.frees==1);teardown(&c,&f);
    printf("capture tests: separated NV12 planes/padding, generation revocation, allocation failure, 24/30fps timed window, duplicate press, deferred cancellation/mode-change/free, stale-ready all passed\n");
    return 0;
}
