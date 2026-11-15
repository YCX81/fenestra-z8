#include "native_capture.h"
#include <stddef.h>
#include "runtime.h"
#define FN(a,t) ((t)(uintptr_t)(a))
static uint32_t read32(uintptr_t address){return *(volatile uint32_t *)address;}
int la_native_geometry(struct la_native_geometry *g) {return la_native_geometry_k(g,1);}
int la_native_geometry_k(struct la_native_geometry *g,unsigned k) {
    if(!g||k>3)return 0;uintptr_t o=(uintptr_t)k*0x100u;
    /* B1 stride/format and matching F1 geometry, from actual runtime regs.
     * These are configuration, NOT evidence of completed frame ownership. */
    g->width=read32(0x1542302c+o);g->height=read32(0x15423030+o);
    g->y_stride=read32(0x15422038+o);g->uv_stride=read32(0x1542203c+o);
    g->nv12=!(read32(0x1542200c+o)&0x10) && read32(0x15422010+o)==1;
    return g->nv12 && g->width>0 && g->height>0 &&
        !((g->width|g->height|g->y_stride|g->uv_stride)&1) &&
        g->width<=4096 && g->height<=4096 && g->y_stride>=g->width &&
        g->uv_stride>=g->width && g->y_stride<=8192 && g->uv_stride<=8192;
}
int la_native_allocate(void *context,uint32_t bytes,void **descriptor,uint8_t **data,uint32_t *actual) {
    (void)context;
    if(!descriptor || !data || !actual || !bytes)return 0;
    *descriptor=0;*data=0;*actual=0;
    /* Ordinary selector only. Eligibility must additionally prohibit HSFC,
     * pre-release and unsupported release modes before calling this. */
    if(read32(UINT64_C(0xa03f31a68))!=0)return 0;
    void *d=FN(UINT64_C(0x4104d2dc),void *(*)(uint32_t,uint32_t,uint32_t))(0x4c41,0x60,bytes);
    if(!d)return 0;
    uintptr_t r=*(const uintptr_t *)((const uint8_t *)d+8);
    uint32_t cls=*(const uint32_t *)((const uint8_t *)d+0x10);
    if(!r || cls!=0x24 || read32(UINT64_C(0xa03f31a68))!=0) {
        la_native_release(0,d);return 0;
    }
    uintptr_t p=*(const uintptr_t *)r;
    uint32_t size=*(const uint32_t *)(r+8);
    if(!p || size<bytes || p>UINTPTR_MAX-size){la_native_release(0,d);return 0;}
    *descriptor=d;*data=(uint8_t *)p;*actual=size;return 1;
}
void la_native_release(void *context,void *descriptor) {
    (void)context;
    if(descriptor)FN(UINT64_C(0x4104d600),void (*)(void *))(descriptor);
}
uint64_t la_native_time_us(void) {
    uint64_t stamp=0;
    FN(UINT64_C(0x40c7c778),void (*)(uint64_t *))(&stamp);
    /* Firmware CNTPCT/24 time units. Existing QEMU 24 MHz uses 41 ns/tick;
     * converts native timestamp to its virtual microseconds, not wall time. */
    return stamp/125*123+(stamp%125)*123/125;
}
struct task_record {
    uint32_t attributes,reserved;uintptr_t context,entry;
    uint32_t priority,reserved2;uint64_t stack_bytes;uintptr_t stack;
    uint32_t processor,affinity;
};
_Static_assert(sizeof(struct task_record)==56,"C3.11 task ABI");
_Static_assert(offsetof(struct task_record,processor)==48,"processor ABI");
int la_native_task_start(struct la_native_task *s,void (*entry)(void *),void *context,
        void *stack,uint32_t bytes,uint32_t priority,uint32_t processor,uint32_t affinity) {
    if(!s || s->phase || !entry || ((uintptr_t)entry&3) || !stack ||
       ((uintptr_t)stack&15) || (bytes&15) || bytes<4096 ||
       (uintptr_t)stack>UINTPTR_MAX-bytes || processor<1 || processor>3 ||
       !(affinity&(1u<<(processor-1))))return 0;
    struct task_record r={0,0,(uintptr_t)context,(uintptr_t)entry,priority,0,
                          bytes,(uintptr_t)stack,processor,affinity};
    s->id=FN(UINT64_C(0x40185064),int32_t (*)(const struct task_record *))(&r);
    if(s->id<1){s->native_error=s->id;s->phase=3;return 0;}
    s->phase=1;
    s->native_error=FN(UINT64_C(0x401855e0),int32_t (*)(int32_t))(s->id);
    if(s->native_error){s->phase=3;return 0;}
    s->phase=2;return 1;
}
int la_native_cpu2_start(struct la_native_task *s,void (*entry)(void *),void *context,
                        void *stack,uint32_t bytes,uint32_t priority) {
    return la_native_task_start(s,entry,context,stack,bytes,priority,3,4);
}
void la_native_yield(void) {FN(UINT64_C(0x40187b00),int32_t (*)(uint32_t))(1);}

/* Best-effort policy, not a native lease. Groups are hypotheses; 0 is the
 * emulator-defined A group, 1 selects B, 2 guesses from submitted bank/row. */
#ifndef LA_STRICT_LEASE
#define LA_STRICT_LEASE 0
#endif
#ifndef LA_PICKER
#define LA_PICKER 0
#endif
static uintptr_t norm(uintptr_t a){return a>=UINT64_C(0xc00000000)?a-UINT64_C(0x400000000):a;}
static int snapshot_pick_c(struct la_capture *c,int record,uint64_t generation, uintptr_t *y, uintptr_t *uv) {
    uint64_t s[33]={0};
    struct live_runtime *r=(struct live_runtime *)((uint8_t *)c-offsetof(struct live_runtime,capture));
    struct live_stream *st=&r->stream;uint32_t mode=st->tune.picker;unsigned ch=r->lv_channel<=3?r->lv_channel:1u;
    FN(UINT64_C(0x40c269cc),void (*)(uintptr_t,void *))(0x15431000,s);
    if(record){uint32_t i=st->snap_head++%16u;st->snap[i].gen=generation;st->snap[i].s1=s[1];st->snap[i].s2=s[2];st->snap[i].s3=s[3];
     st->snap[i].s4=s[4];st->snap[i].s11=s[11];st->snap[i].s20=s[20];st->snap[i].s27=s[27];
     for(unsigned k=0;k<4;k++)r->lv_snap[k]=s[3+k];
     if(ch!=r->lv_pad){st->diag.nbufs=0;st->trans_n=0;st->last_y=0;st->last_kept_y=0;r->lv_pad=ch;} /* v17: new channel, new buffers */
     uintptr_t cur=norm(s[3+ch]);
     if(st->last_y && generation==st->last_gen+1 && cur!=st->last_y){unsigned k;for(k=0;k<st->trans_n&&st->trans[k].from!=st->last_y;k++){}
      if(k<8){st->trans[k].from=st->last_y;st->trans[k].to=cur;if(k==st->trans_n)st->trans_n++;}}
     st->last_y=cur;st->last_gen=generation;
     /* v12: the buffer set comes from the snapshot word itself, never from the picked address. */
     {unsigned k;for(k=0;k<st->diag.nbufs&&st->diag.bufs[k]!=cur;k++){}
      if(k==st->diag.nbufs&&st->diag.nbufs<8&&cur)st->diag.bufs[st->diag.nbufs++]=cur;}}
#if LA_PICKER == 0
    if(mode==1){*y=s[20];*uv=s[27];}
    else{*y=s[3+ch];*uv=s[10+ch];} /* C0+a0/a8 (channel 1); v14: channel ch */
    if(mode==2||mode==3){
     uintptr_t cur=norm(*y),d=norm(*uv)-cur,prev=0;
     if(mode==2){for(unsigned k=0;k<st->trans_n;k++)if(st->trans[k].to==cur){prev=st->trans[k].from;break;}}
     else{uintptr_t best=0,mx=0;for(unsigned k=0;k<st->diag.nbufs;k++){uintptr_t b=st->diag.bufs[k];if(b>mx)mx=b;if(b<cur&&b>best)best=b;}
      prev=best?best:(mx&&mx!=cur?mx:0);}
     if(prev){*y=prev;*uv=prev+d;if(record)st->picked_prev++;}else if(record)st->picked_fallback++;
    }
#elif LA_PICKER == 1
    *y=s[20];*uv=s[27]; /* C0+120/128 */
#elif LA_PICKER == 2
    uintptr_t bank=read32(0x15431008)&1;
    uintptr_t row=(generation-1)&3; /* submitted-row timing remains a guess */
    uintptr_t a=0x15431230+bank*0x200+row*0x80;
    *y=read32(a)|((uint64_t)(read32(a+4)&15)<<32);
    *uv=read32(a+8)|((uint64_t)(read32(a+12)&15)<<32);
#else
#error unsupported LA_PICKER
#endif
    if(*y>=UINT64_C(0xc00000000)) *y-=UINT64_C(0x400000000);
    if(*uv>=UINT64_C(0xc00000000)) *uv-=UINT64_C(0x400000000);
    /* v17: work RAM, or the image banks (channel 2 buffers) */
    #define LA_RAM(a) (((a)>=UINT64_C(0xa00000000)&&(a)<UINT64_C(0xa40000000))||((a)>=UINT64_C(0x840000000)&&(a)<UINT64_C(0x8fff47400)))
    return LA_RAM(*y) && LA_RAM(*uv);
}
#if !LA_STRICT_LEASE
static void invalidate(uintptr_t p,size_t n) {
    uintptr_t end=(p+n+63)&~(uintptr_t)63;
    for(p&=~(uintptr_t)63;p<end;p+=64)
        __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif
int la_best_begin(void *ctx,uint64_t generation,uint64_t timestamp,struct la_source *s) {
#if LA_STRICT_LEASE
    (void)ctx;(void)generation;(void)timestamp;(void)s;return 0;
#else
    struct la_capture *c=ctx;struct la_native_geometry g;uintptr_t y,uv;
    uint32_t epoch=__atomic_load_n(&c->cancel_epoch,__ATOMIC_ACQUIRE);
    struct live_runtime *rt=(struct live_runtime *)((uint8_t *)c-offsetof(struct live_runtime,capture));
    if(!la_native_geometry_k(&g,rt->lv_channel<=3?rt->lv_channel:1u)||!snapshot_pick_c(c,1,generation,&y,&uv))return 0;
    size_t yn=(size_t)g.y_stride*g.height,un=(size_t)g.uv_stride*(g.height/2);
    if(y+yn>UINT64_C(0xa40000000)||uv+un>UINT64_C(0xa40000000))return 0;
    invalidate(y,yn);invalidate(uv,un);
    *s=(struct la_source){(uint8_t *)y,(uint8_t *)uv,g.width,g.height,g.y_stride,g.uv_stride,generation,timestamp,epoch};
    return 1;
#endif
}
int la_best_valid(void *ctx,const struct la_source *s) {
    struct la_capture *c=ctx;uintptr_t y,uv;
    uint64_t seq=__atomic_load_n(&c->irq_seq,__ATOMIC_ACQUIRE);
    return !(seq&1) && seq==s->generation*2 &&
        __atomic_load_n(&c->cancel_epoch,__ATOMIC_ACQUIRE)==s->lease &&
        snapshot_pick_c(c,0,s->generation,&y,&uv) && y==(uintptr_t)s->y && uv==(uintptr_t)s->uv;
}
void la_best_end(void *ctx,const struct la_source *s){(void)ctx;(void)s;}

/* Small non-DMA heap allocation for the reduced-rate ring. */
int la_heap_allocate(void *ctx,uint32_t bytes,void **descriptor,uint8_t **data,uint32_t *actual) {
 (void)ctx;if(!descriptor||!data||!actual||!bytes)return 0;
 void *p=FN(UINT64_C(0x40d2c408),void *(*)(uint32_t,uint32_t))(bytes,0);
 if(!p)return 0;*descriptor=p;*data=p;*actual=bytes;return 1;
}
void la_heap_release(void *ctx,void *p) {(void)ctx;if(p)FN(UINT64_C(0x40d2c448),void (*)(void *))(p);}
