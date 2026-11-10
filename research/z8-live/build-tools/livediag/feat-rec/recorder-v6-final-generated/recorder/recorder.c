/* Diagnostic only. Native I/O has NO proven bounded latency or durable flush. */
#include "recorder.h"
#include "lamp_protocol.h"
#include "../runtime.h"
#include "../capture/live_alias.h"
#define F(a,t) ((t)(uintptr_t)UINT64_C(a))
#define R32(a) (*(volatile uint32_t *)(uintptr_t)UINT64_C(a))
#define DELAY F(0x40187b00,int32_t(*)(uint32_t))
#define OPEN F(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))
#define WRITE F(0x402dd78c,uint32_t(*)(int32_t,const void*,uint32_t,uint32_t*,void*))
#define SEEK F(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))
#define CLOSE F(0x402dd558,uint32_t(*)(int32_t))
static const uint32_t sites[16]={[FR_ENTRY]=0x402f2d58};
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
static uint32_t checksum(const uint8_t *p){uint32_t h=2166136261u;for(unsigned i=0;i<508;i++)h=(h^p[i])*16777619u;return h;}
static struct live_runtime *runtime(struct fr_state *s){
 return (void*)((uint8_t*)s-__builtin_offsetof(struct live_runtime,recorder));
}
static void record(struct fr_state *s,unsigned event,const uint64_t *regs,uint32_t why){
 uint32_t *p=(void*)s->io;for(unsigned i=0;i<128;i++)p[i]=0;
 p[0]=FR_MAGIC;p[1]=FR_VERSION;p[2]=s->seq;p[3]=event;p[4]=sites[event];
 qword(p+6,ticks());qword(p+8,s->session);p[10]=(regs&&event==FR_ENTRY)?(uint32_t)regs[0]:s->client;
 if(regs){p[11]=(uint32_t)regs[0];qword(p+12,regs[0]);qword(p+14,regs[19]);qword(p+16,regs[30]);}
 p[18]=s->tx[0].error;p[19]=s->tx[0].written;p[20]=s->tx[0].close_rc;p[21]=s->epoch;
 p[22]=s->kbps;p[23]=s->size;qword(p+24,s->arena);
 p[26]=LOAD(&s->missed);p[27]=LOAD(&s->seen);
 if(event==FR_ENTRY){
  /* Lightweight records carry all prior native results, without Live reads. */
  for(unsigned i=0;i<FR_SLOTS;i++){
   const struct fr_tx *t=&s->tx[i];unsigned j=28+i*6;
   p[j]=t->event;p[j+1]=LOAD(&t->phase);p[j+2]=t->error;
   p[j+3]=t->written;p[j+4]=t->close_rc;p[j+5]=t->serial;
  }
 }
 p[5]=LOAD(&runtime(s)->shutdown);p[86]=6;p[87]=LOAD(&s->arm);p[88]=LOAD(&s->skip_nr);
 p[117]=R32(0xa03bd062c);p[118]=s->generation;
 p[125]=why;p[126]=LOAD(&runtime(s)->card_epoch);
 /* Prior transactions' latest phases, ordered by fixed slot, not chronology. */
 for(unsigned i=0;i<FR_SLOTS;i++)p[119+i]=LOAD(&s->tx[i].phase);
 /* Current slot phase is sampled BEFORE this transaction, never a return proof. */
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
 char path[]="A:\\W5L00000.BIN";unsigned token=s->generation;
 for(int i=10;i>=6;i--){path[i]=(char)('0'+token%10);token/=10;}
 if(!s->generation || s->generation>99999 || (!create && !s->file_owned)){t->event=event;t->error=0xf004u;STORE(&t->phase,18);return 0;}
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
 if(!token || token>99999 || token<=LOAD(&s->attempted) || LOAD(&s->claimed) || LOAD(&s->entry_claimed))return;
 if(!take(s))return;
 /* Worker rechecks eligibility; mode can still change after this check. */
 if(LOAD(&s->arm)!=token || LOAD(&s->claimed) || LOAD(&s->entry_claimed) || R32(0xa03bd062c)!=0 ||
    LOAD(&r->shutdown)||LOAD(&r->writer_active)||LOAD(&r->stream.job)||LOAD(&r->capture.phase)!=LA_READY){give(s);return;}
 STORE(&s->ready,0);s->file_owned=0;STORE(&s->attempted,token);STORE(&s->generation,token);
 STORE(&s->epoch,LOAD(&r->card_epoch));s->session=ticks();s->seq=0;
 for(unsigned i=0;i<FR_SLOTS;i++)s->tx[i]=(struct fr_tx){0};
 for(unsigned i=0;i<FR_BYTES;i++)s->io[i]=0;
 record(s,0,0,0);
 if(persist(s,1,0)){
  s->file_owned=1; /* Exclusive OPEN, full WRITE and successful CLOSE required. */
  if(LOAD(&s->arm)==token)STORE(&s->ready,1);
 }
 give(s);
}
/* Claim once after exact LR match. No allocation, file I/O or Live snapshot.
 * The assembly immediately calls fr_light(I) with this pinned identity. */
struct fr_state *fr_before(const uint64_t *r){
 struct fr_state *s=fr_context();
 if(!s || !r || r[30]!=UINT64_C(0x40d07ba8))return 0;
 uint32_t zero=0;
 if(!__atomic_compare_exchange_n(&s->entry_claimed,&zero,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
  /* M may have been skipped while unarmed. A second exact-LR invocation must
   * never inherit an unfinished session on a reused SP. Conservative cutoff
   * also drops remaining codes if two such entries actually overlap. */
  __atomic_fetch_or(&s->seen,FR_DONE,__ATOMIC_RELEASE);return 0;
 }
 __atomic_fetch_and(&s->lamp_seen,~(3u<<FR_NR_SHIFT),__ATOMIC_ACQ_REL);
 s->client=(uint32_t)r[0];s->kbps=(uint32_t)r[1];s->arena=r[2];s->size=(uint32_t)r[3];
 s->movrs_sp=(uintptr_t)(r+32);
 __atomic_fetch_or(&s->seen,1u<<FR_ENTRY,__ATOMIC_RELEASE);
 STORE(&s->claimed,1);
 return s;
}
/* Same rejection predicates as stock DELAY's 0x40187b0c..0x40187b54:
 * task context, IRQ unmasked, scheduler available. Never wait after DELAY
 * reports an error (otherwise a rejected sleep could become a busy loop). */
static int sleepable(void){
 uint64_t flags,tls;
 __asm__ volatile("mrs %0,daif; mrs %1,tpidr_el1":"=r"(flags),"=r"(tls));
 if((flags&0x80u) || !tls)return 0;
 return !*(volatile uint32_t *)(uintptr_t)(tls+0x50) &&
          *(volatile uint32_t *)(uintptr_t)(tls+0x14)!=0;
}
static uint64_t frequency(void){uint64_t f;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(f));return f;}
static int cancelled(struct fr_state *s){
 return s && ((LOAD(&s->missed)&FR_RETURNED) || (LOAD(&s->seen)&FR_DONE));
}
static int pulse(uint32_t level,uint32_t ms,uint64_t freq,struct fr_state *cancel){
 if(cancelled(cancel))return 0;
 lamp(level);
 /* Per-edge absolute deadline: late scheduling stretches, never compresses,
  * a half-bit. GPIO/call overhead and scheduler lateness are additional time. */
 uint64_t end=ticks()+(freq*ms+999u)/1000u;
 while((int64_t)(ticks()-end)<0){if(cancelled(cancel) || DELAY(1)!=0)return 0;}
 return 1;
}
static int emit_frame(uint32_t stage,uint32_t value,struct fr_state *cancel){
 unsigned n;uint64_t bits=fr_lamp_bits(stage,value,&n),freq=frequency();
 if(!freq || !sleepable())return 0;
 uint32_t half=stage==FR_D?25u:50u;
 int ok=pulse(0,half*4,freq,cancel)&&pulse(1,half*5,freq,cancel)&&pulse(0,half*3,freq,cancel);
 for(unsigned i=n;ok && i;i--){
  uint32_t bit=(uint32_t)(bits>>(i-1))&1u;
  ok=pulse(!bit,half,freq,cancel)&&pulse(bit,half,freq,cancel);
 }
 lamp(0);return ok;
}
static int emit(uint32_t stage,uint32_t value){return emit_frame(stage,value,0);}
/* Exclusive lamp ownership across close callers and I/R/S/M. No native lock.
 * Reporters never wait. R cancels a reporter; hook waits at most 100 ms before
 * failing closed (no GPIO). A dead/starved reporter cannot hang returned REC. */
static int lamp_take(struct fr_state *s){
 return !(__atomic_fetch_or(&s->missed,FR_LAMP_BUSY,__ATOMIC_ACQ_REL)&FR_LAMP_BUSY);
}
static void lamp_give(struct fr_state *s){__atomic_fetch_and(&s->missed,~FR_LAMP_BUSY,__ATOMIC_RELEASE);}
static int hook_take(struct fr_state *s){
 if(lamp_take(s))return 1;
 uint64_t f=frequency(),start=ticks();
 if(!f || !sleepable())return 0;
 do {
  if(DELAY(1)!=0)return 0;
  if(lamp_take(s))return 1;
 } while(ticks()-start<f/10u);
 return 0;
}
void fr_checkpoint(uint32_t stage){
 /* Called BEFORE w5_diag reserves/writes a RAM record, including when its
  * finite log is full. Only selected existing close milestones are emitted.
  * No encoder pointer, native lock, register snapshot or file operations. */
 if(!(stage==1 || stage==2 || stage==10 || stage==11 || stage==13 ||
      stage==15 || stage==17 || stage==18))return;
 struct fr_state *s=fr_context();
 if(!s || !LOAD(&s->claimed) || !(LOAD(&s->lamp_seen)&(1u<<FR_I)) || cancelled(s))return;
 if(!lamp_take(s))return;
 if(!cancelled(s)){
  /* D value: existing w5_diag stage[5] and actual skip mask[2]. */
  uint32_t value=(stage<<2)|((LOAD(&s->lamp_seen)>>FR_NR_SHIFT)&3u);
  if(!emit_frame(FR_D,value,s) && !cancelled(s))
   __atomic_fetch_or(&s->lamp_failed,1u<<FR_D,__ATOMIC_RELAXED);
 }
 lamp_give(s);
}
static int matches(struct fr_state *s,uint32_t stage,const uint64_t *r){
 if(!s || !r || stage>FR_M || !LOAD(&s->claimed) || (LOAD(&s->seen)&FR_DONE))return 0;
 uintptr_t sp=(uintptr_t)(r+32);
 if(stage<=FR_R)return sp==s->movrs_sp && r[30]==UINT64_C(0x40d07ba8);
 /* Both stock points are inside the same 0x80-byte bring-up frame. Check SP
  * BEFORE reading the caller's stack; compare full saved LR, u32 W19 client.
  * M's live LR depends on bind/error path, so only S checks live X30. */
 return sp+0x80==s->movrs_sp && (uint32_t)r[19]==s->client &&
        *(const uint64_t *)(sp+0x78)==UINT64_C(0x40d07ba8) &&
        (stage!=FR_S || r[30]==UINT64_C(0x402f303c));
}
void fr_light(struct fr_state *s,uint32_t stage,const uint64_t *r){
 if(!matches(s,stage,r))return;
 uint32_t bit=1u<<stage;
 if(__atomic_fetch_or(&s->lamp_seen,bit,__ATOMIC_ACQ_REL)&bit)return;
 uint32_t value=stage==FR_S?(uint32_t)r[0]:0;
 if(stage==FR_R)value=(LOAD(&s->lamp_seen)>>FR_NR_SHIFT)&3u;
 if(stage==FR_M){
  /* Consume at the convergence point even if S was absent; never let a later
   * REC on the same stack inherit this boot's diagnostic. Five stock clients. */
  __atomic_fetch_or(&s->seen,FR_DONE,__ATOMIC_RELEASE);
  if((uint32_t)r[19]>4u){__atomic_fetch_or(&s->lamp_failed,bit,__ATOMIC_RELAXED);return;}
  value=((volatile uint8_t *)(uintptr_t)UINT64_C(0xa02e98480))[(uint32_t)r[19]];
 }
 /* Publish return before waiting: no future D can start; in-flight D aborts. */
 if(stage==FR_R)__atomic_fetch_or(&s->missed,FR_RETURNED,__ATOMIC_RELEASE);
 if(!hook_take(s)){__atomic_fetch_or(&s->lamp_failed,bit,__ATOMIC_RELAXED);return;}
 /* Value captured before the first GPIO/DELAY, especially M before its LDP. */
 if(!emit(stage,value))__atomic_fetch_or(&s->lamp_failed,bit,__ATOMIC_RELAXED);
 lamp_give(s);
}
void fr_probe(uint32_t stage,const uint64_t *r){
 /* I/R use the pointer pinned by the assembly, not this mutable arm lookup. */
 if(stage==FR_S || stage==FR_M)fr_light(fr_context(),stage,r);
}

/* Only the Live close call is redirected here. Native EXIT is never patched. */
extern const char fr_exit_return[], fr_nr_lr_a[], fr_nr_lr_b[];
_Static_assert(__builtin_offsetof(struct fr_state,live_exit_active)==88,"NR owner ABI");
_Static_assert(__builtin_offsetof(struct fr_state,skip_nr)==100,"NR switch ABI");
uint64_t fr_live_exit(uint32_t client){
 struct fr_state *s=fr_context();
 if(client==4 && s && LOAD(&s->skip_nr)==1)return fr_exit_scoped(s);
 return F(0x402f3084,uint64_t(*)(uint32_t))(client);
}
int fr_nr_begin(struct fr_state *s,uintptr_t sp){
 if(!LOAD(&s->arm) || LOAD(&s->skip_nr)!=1)return 0;
 uint64_t zero=0;
 return __atomic_compare_exchange_n(&s->live_exit_active,&zero,sp,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
void fr_nr_end(struct fr_state *s){STORE(&s->live_exit_active,0);}
int fr_nr_allow(struct fr_state *s,const uint64_t *r,uint64_t lr){
 uintptr_t sp=(uintptr_t)(r+32);
 if(!LOAD(&s->arm) || LOAD(&s->skip_nr)!=1 || LOAD(&s->live_exit_active)!=sp)return 0;
 /* Check exact SP before dereferencing the native 32-byte EXIT frame. */
 if(*(const uint64_t *)(sp+24)!=(uintptr_t)fr_exit_return || r[30]!=lr)return 0;
 uint32_t id=(uint32_t)r[1];
 if(!((id==0x4a && lr==(uintptr_t)fr_nr_lr_a) ||
      (id==0x4b && lr==(uintptr_t)fr_nr_lr_b)))return 0;
 __atomic_fetch_or(&s->lamp_seen,1u<<(FR_NR_SHIFT+id-0x4a),__ATOMIC_RELEASE);
 return 1;
}
