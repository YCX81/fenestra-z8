/* Diagnostic only. Native I/O has NO proven bounded latency or durable flush. */
#include "recorder.h"
#include "../runtime.h"
#include "../capture/live_alias.h"
#define F(a,t) ((t)(uintptr_t)UINT64_C(a))
#define R32(a) (*(volatile uint32_t *)(uintptr_t)UINT64_C(a))
#define R64(a) (*(volatile uint64_t *)(uintptr_t)UINT64_C(a))
#define OPEN F(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))
#define WRITE F(0x402dd78c,uint32_t(*)(int32_t,const void*,uint32_t,uint32_t*,void*))
#define SEEK F(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))
#define CLOSE F(0x402dd558,uint32_t(*)(int32_t))
static const uint32_t sites[15]={[1]=0x402f2d58,[2]=0x402f303c,[4]=0x402f5870,
 [5]=0x402f58cc,[13]=0x402f4c1c,[14]=0x402f4560};
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
static void lamp(uint32_t on){F(0x40c15c3c,void(*)(void*,uint32_t))((void*)(uintptr_t)UINT64_C(0xa0280a1b0),on);}
static uint64_t ticks(void){uint64_t t;__asm__ volatile("mrs %0,cntpct_el0":"=r"(t));return t;}
/* One exclusive attempt. No spin, DELAY, blocking mutex, or retry on contention. */
static int take(struct fr_state *s){uint32_t old,fail;
 __asm__ volatile("ldaxr %w0,[%2]\n cbnz %w0,1f\n stlxr %w1,%w3,[%2]\n b 2f\n1: clrex\n mov %w1,#1\n2:"
 :"=&r"(old),"=&r"(fail):"r"(&s->lock),"r"(1u):"memory");return !fail;}
static void give(struct fr_state *s){__atomic_store_n(&s->lock,0,__ATOMIC_RELEASE);}
static void qword(uint32_t *p,uint64_t v){p[0]=(uint32_t)v;p[1]=(uint32_t)(v>>32);}
static void copy32(uint32_t *p,uint64_t at,unsigned n){for(unsigned i=0;i<n;i++)p[i]=((volatile uint32_t*)(uintptr_t)at)[i];}
static uint32_t checksum(const uint8_t *p){uint32_t h=2166136261u;for(unsigned i=0;i<508;i++)h=(h^p[i])*16777619u;return h;}
static void record(struct fr_state *s,unsigned event,const uint64_t *regs){
 uint32_t *p=(void*)s->io;for(unsigned i=0;i<128;i++)p[i]=0;
 p[0]=FR_MAGIC;p[1]=2;p[2]=s->seq;p[3]=event;p[4]=sites[event];
 qword(p+6,ticks());qword(p+8,s->session);p[10]=s->client;
 if(regs){p[11]=(uint32_t)regs[0];qword(p+12,regs[0]);qword(p+14,regs[19]);qword(p+16,regs[30]);}
 p[18]=s->tx[0].error;p[19]=s->tx[0].written;p[20]=s->tx[0].close_rc;p[21]=s->epoch;
 p[22]=s->kbps;p[23]=s->size;qword(p+24,s->arena);
 p[26]=LOAD(&s->missed);p[27]=LOAD(&s->seen);
 if(event){
  /* Live fields [28..43]; fixed stock RAM only, no VPU MMIO and no pointer chasing. */
  fr_encoder_snapshot(p+28);
  copy32(p+44,UINT64_C(0xa02d3274c),2); /* worker states */
  copy32(p+46,UINT64_C(0xa02d30118),6); /* both {used,A,B} */
  copy32(p+52,UINT64_C(0xa02d30130),2);p[54]=R32(0xa02d30110);
  p[55]=R32(0xa02d30148);qword(p+56,R64(0xa02d30150));
  copy32(p+58,UINT64_C(0xa02d3d928),24); /* VDI +0..0x5f, base at +0x58 */
  for(unsigned i=0;i<5;i++)p[82+i]=((volatile uint8_t*)(uintptr_t)UINT64_C(0xa02e98480))[i];
  copy32(p+88,UINT64_C(0xa02d32758),4); /* instance pointers, never dereference */
  for(unsigned i=0;i<4;i++){ /* Four static slots: first four words + context pointer */
   copy32(p+92+i*6,UINT64_C(0xa02d41848)+i*0x30,4);
   copy32(p+96+i*6,UINT64_C(0xa02d41868)+i*0x30,2);
  }
  p[116]=R32(0xa02e98de8);p[117]=R32(0xa03bd062c);
 }
 p[118]=s->generation;
 /* Prior transactions' latest phase; current WRITE_ENTER is a prediction until RAM confirms it. */
 for(unsigned i=0;i<4;i++)p[119+i]=LOAD(&s->tx[i].phase);
 p[123]=s->seq;p[124]=11; /* current write-enter, not write-return */
 p[127]=checksum(s->io);
}
static const void *native_buffer(struct fr_state *s,uint32_t n){uintptr_t p=(uintptr_t)s->io;
 la_dc_cvac(p,n);return (void*)(p>=UINT64_C(0xc00000000)&&p<UINT64_C(0x1000000000)?p-UINT64_C(0x400000000):p);}
/* RAM phases: odd = about to call, even = returned. No extra progress writes.
 * 1/2 OPEN; 3/4 EOF SEEK; 5/6 offset SEEK; 11/12 WRITE; 13/14 CLOSE;
 * 16 transaction success, 18 returned failure. No timeout is implied. */
static void phase(struct fr_tx *t,uint32_t p){STORE(&t->phase,p);}
static int persist(struct fr_state *s,int create,unsigned event){
 struct fr_tx *t=&s->tx[s->seq];
 uint32_t error=0,written=0,closed=0;uint64_t actual=0;
 char path[]="A:\\W5F00000.BIN";unsigned token=s->generation;
 for(int i=10;i>=6;i--){path[i]=(char)('0'+token%10);token/=10;}
 const void *buf=native_buffer(s,create?FR_BYTES:512);
 t->event=event;t->serial=s->seq+1;lamp(1);phase(t,1);
 int32_t fd=OPEN(path,create?0x101u:1u,0x20u);phase(t,2);
 if(fd<0 || (uint32_t)fd==0x7fff0000u)error=(uint32_t)fd;
 else {
  if(!create){phase(t,3);uint32_t rc=SEEK(fd,0,2,&actual);phase(t,4);
   if(!rc||actual!=FR_BYTES)error=0xf001u;}
  if(!error && !create){phase(t,5);uint32_t rc=SEEK(fd,(int64_t)s->seq*512,0,&actual);phase(t,6);
   if(!rc||actual!=(uint64_t)s->seq*512)error=0xf002u;}
  if(!error){phase(t,11);error=WRITE(fd,buf,create?FR_BYTES:512,&written,0);phase(t,12);
   if(!error && written!=(create?FR_BYTES:512))error=0xf003u;}
  t->error=error;t->written=written;
  phase(t,13);closed=CLOSE(fd);t->close_rc=closed;phase(t,14);
 }
 t->error=error;t->written=written;t->close_rc=closed;
 phase(t,(!error&&!closed)?16:18);lamp(0);
 return !error&&!closed;
}
void fr_prepare(struct live_runtime *r){
 struct fr_state *s=&r->recorder;uint32_t token=LOAD(&s->arm);
 if(!token || token>99999 || token<=LOAD(&s->attempted) || LOAD(&s->claimed))return;
 if(!take(s))return;
 /* Worker rechecks eligibility; mode can still change after this check. */
 if(LOAD(&s->arm)!=token || LOAD(&s->claimed) || R32(0xa03bd062c)!=0 ||
    LOAD(&r->shutdown)||LOAD(&r->writer_active)||LOAD(&r->stream.job)||LOAD(&r->capture.phase)!=LA_READY){give(s);return;}
 STORE(&s->ready,0);STORE(&s->attempted,token);STORE(&s->generation,token);
 STORE(&s->epoch,LOAD(&r->card_epoch));s->session=ticks();s->seq=0;
 for(unsigned i=0;i<4;i++)s->tx[i]=(struct fr_tx){0};
 for(unsigned i=0;i<FR_BYTES;i++)s->io[i]=0;
 record(s,0,0);
 if(persist(s,1,0) && LOAD(&s->arm)==token){s->seq=1;STORE(&s->ready,1);}
 give(s);
}
/* One REC session per boot: immutable identity after publication. A new arm
 * token can retry preparation, never reuse a REC generation or a stuck lock. */
static void begin(struct fr_state *s,const uint64_t *r){
 uint32_t zero=0;
 if(!LOAD(&s->ready) || r[30]!=0x40d07ba8 || r[0]>1 || R32(0xa03bd062c)==0)return;
 if(!__atomic_compare_exchange_n(&s->claimed,&zero,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
 /* ready publishes completed preparation. Identity is single-writer after
  * claimed CAS, independent of the transaction lock (which may still be held
  * by the preparing worker for its final give). */
 if(!LOAD(&s->ready)||LOAD(&s->arm)!=LOAD(&s->generation))return;
 s->client=(uint32_t)r[0];s->kbps=(uint32_t)r[1];s->arena=r[2];s->size=(uint32_t)r[3];
 s->movrs_sp=(uintptr_t)(r+32);
 STORE(&s->seen,1u<<1);
 if(!take(s)){__atomic_fetch_or(&s->missed,1u<<1,__ATOMIC_RELAXED);return;}
 record(s,1,r);if(persist(s,0,1))s->seq++;else STORE(&s->ready,0);
 give(s);
}
/* Called by the integer-only assembly gate, before SIMD save. Read-only. */
static uint32_t matches(struct fr_state *s,uint32_t event,const uint64_t *r){
 if(!s || !r)return 0;
 /* Start-bearing callbacks must reach the atomic consumer even if mode or
  * provenance rejects association. Never strand a token in the outer gate. */
 if(event==14)return (r[0]&1) && LOAD(&s->pending)!=0;
 if(R32(0xa03bd062c)==0)return 0;
 if(event==1)return LOAD(&s->ready) && !LOAD(&s->claimed) && r[30]==0x40d07ba8 && r[0]<=1;
 uint32_t seen=LOAD(&s->seen),gen=LOAD(&s->generation);
 if(event>14 || (seen&(1u<<event)) || !(seen&(1u<<1)) ||
    (seen&FR_DONE) || LOAD(&s->arm)!=gen)return 0;
 uintptr_t sp=(uintptr_t)(r+32);
 if(event==2)return sp+0x80==s->movrs_sp && r[19]==s->client &&
   *(const uint64_t *)(sp+0x78)==0x40d07ba8 && r[30]==0x402f303c;
 if(event==4)return sp==s->movrs_sp && r[30]==0x40d07bb0;
 if(event==13)return LOAD(&s->adapter_gen)==gen && sp+0x1a0==s->adapter_sp &&
   (r[19]&1) && r[30]==0x402f4c1c;
 if(event==5)return sp+16==s->movrs_sp && r[30]==0x402f58cc &&
   *(const uint64_t *)(sp+8)==0x40d07bb0;
 return 0;
}
uint32_t fr_enabled(uint32_t event,const uint64_t *r){return matches(fr_context(),event,r);}
void fr_probe(uint32_t event,const uint64_t *r){
 struct fr_state *s=fr_context();if(!matches(s,event,r))return;
 if(event==1){begin(s,r);return;}
 uint32_t gen=LOAD(&s->generation),seen;
 if(event==4)STORE(&s->pending,gen);
 if(event==14){
  uint32_t pending=__atomic_exchange_n(&s->pending,0,__ATOMIC_ACQ_REL);
  if(!pending)return;
  if(pending!=gen || r[0]!=1 || R32(0xa03bd062c)==0 ||
     (LOAD(&s->seen)&FR_DONE) || LOAD(&s->adapter_gen)){
   /* Immutable claimed identity cannot ever bind a later REC callback. */
   __atomic_fetch_or(&s->seen,FR_DONE,__ATOMIC_RELEASE);
   STORE(&s->ready,0);STORE(&s->adapter_gen,0);return;
  }
  s->adapter_sp=(uintptr_t)(r+32);STORE(&s->adapter_gen,gen);
 }
 /* Arrival and termination NEVER depend on the I/O lock or ready. */
 uint32_t bit=1u<<event;
 seen=__atomic_fetch_or(&s->seen,bit,__ATOMIC_ACQ_REL)|bit;
 if((seen&((1u<<5)|(1u<<13)))==((1u<<5)|(1u<<13)))
  __atomic_fetch_or(&s->seen,FR_DONE,__ATOMIC_RELEASE);
 if(event!=2 && event!=13)return; /* RAM-only provenance/lifecycle probes */
 if(!LOAD(&s->ready) || !take(s)){__atomic_fetch_or(&s->missed,bit,__ATOMIC_RELAXED);return;}
 if(s->seq<4 && LOAD(&s->arm)==gen && LOAD(&s->ready)){
  record(s,event,r);if(persist(s,0,event))s->seq++;else STORE(&s->ready,0);
 }
 give(s);
}
