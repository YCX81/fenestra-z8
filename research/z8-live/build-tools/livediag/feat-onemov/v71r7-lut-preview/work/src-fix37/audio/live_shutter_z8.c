#include "../runtime.h"
#include "../ui/live_ui.h"
#define FN(a,t) ((t)(uintptr_t)(a))
#define LOAD(p) __atomic_load_n(p,__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define OR(p,v) __atomic_fetch_or(p,v,__ATOMIC_ACQ_REL)
#if defined(__APPLE__) || defined(LIVE_HOST_TEST)
#define SH __attribute__((noinline))
#else
#define SH __attribute__((section(".live_shutter"),noinline))
#endif
#define START 0x04100030u
#define DONE 0x40f9d194u
#define DISPATCH 0x40e34b2cu
/* wake_generation holds independent lower-queue ownership and replies.
 * No timeout makes an outstanding message reusable. */
enum { SETUP=1,GO=2,STOP=4,SETUP_ACK=8,GO_ACK=16,STOP_ACK=32,
 BARRIER=64,RETURNED=128,START_REPLY=256,DONE_REPLY=512,
 START_SENT=1024,DONE_SENT=2048,QUEUED=4096,FAILED=8192,
 EVENT_SEEN=16384,FENCE_SENT=32768,FENCE_ACK=65536,EVENT_ARMED=131072 };
typedef void (*reply_fn)(uint32_t,void *);
static uint32_t word(uintptr_t p){return *(volatile uint32_t *)p;}
static uintptr_t ptr(uintptr_t p){return *(volatile uintptr_t *)p;}
static SH struct live_runtime *runtime(void){
 struct live_runtime*r=live_ui_engine_get();
 return r&&r->magic==LIVE_RUNTIME_MAGIC&&r->size==sizeof *r?r:0;
}
static SH int wanted(struct live_runtime*r){
 return r&&!LOAD(&r->shutdown)&&LOAD(&r->requested)&&!LOAD(&r->crumb_pad)&&
  !word(UINT64_C(0xa03bd062c))&&r->audio.active&&!LOAD(&r->audio.cancelled);
}
static SH int allowed(struct live_runtime*r){
 return wanted(r)&&!r->audio_error&&!r->shutter.outer[2]&&!live_shutter_mute_get()&&
  !*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc7008);
}
static SH int stopped(struct live_shutter_z8*s){
 uint32_t f=LOAD(&s->wake_generation);
 if(!(f&GO))return !(f&SETUP)||(f&SETUP_ACK);
 return (f&(STOP_ACK|BARRIER|GO_ACK|FENCE_ACK))==(STOP_ACK|BARRIER|GO_ACK|FENCE_ACK)&&
   (!(f&SETUP)||(f&SETUP_ACK));
}
static SH uint32_t hard_stop(struct live_shutter_z8*s){
 uint32_t zero=0;
 if(!__atomic_compare_exchange_n(&s->cancel_posted,&zero,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 1;
 uintptr_t*outer=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(24,0);
 if(outer){
  struct laz8_msg*m=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(56,0);
  if(m){
   outer[0]=outer[2]=0;outer[1]=0x40e3159cu;
   m->id=START+2;m->param=0x40e32ed8u;m->payload=outer;m->tail=(uintptr_t)m;
   if(!FN(0x40c00bac,int(*)(void*,void*))((void*)ptr(UINT64_C(0xa038fea70)),m))return 1;
   FN(0x40d2c448,void(*)(void*))(m);
  }
  FN(0x40d2c448,void(*)(void*))(outer);
 }
 s->errors++;OR(&s->wake_generation,FAILED);STORE(&s->cancel_posted,0);return 0;
}
__attribute__((noinline)) uint32_t live_shutter_request_stop(void){
 struct live_runtime*r=runtime();
 if(r&&LOAD(&r->shutter.phase))return hard_stop(&r->shutter);
 return FN(0x40e316bc,uint32_t(*)(void))();
}
/* Native codec task dispatcher consumes the whole message synchronously under
 * its original mutex. Only output volume, DAC GO and DAC STOP are admitted;
 * no prepare/route script and no shared high-driver 109/105/108 service. */
static SH void codec(uint32_t id,void*p){
 struct laz8_msg m={0};m.id=id;m.payload=p;
 FN(0x40c7e658,void(*)(void*))(&m);
}
static SH int post(struct laz8_msg*m,uint32_t id,void*p,struct live_shutter_z8*s);
static SH void queue_reply(struct live_shutter_z8*s){
 uint32_t f=LOAD(&s->wake_generation);
 if(!s->held||!(f&RETURNED)||(f&QUEUED)||((f&GO)&&!(f&GO_ACK))||
    ((f&SETUP)&&!(f&SETUP_ACK)))return;
 uint32_t old=OR(&s->wake_generation,QUEUED);
 if(old&QUEUED)return;
 if(FN(0x40c00bac,int(*)(void*,void*))((void*)ptr(UINT64_C(0xa038fea70)),(void*)s->held))
  __atomic_fetch_and(&s->wake_generation,~QUEUED,__ATOMIC_RELEASE);
}
static SH void ack(uint32_t id,void*p){
 struct live_shutter_z8*s=p;
 uint32_t bit=id==0x42040708u?((LOAD(&s->wake_generation)&FENCE_SENT)?FENCE_ACK:SETUP_ACK):id==0x42040702u?GO_ACK:STOP_ACK;
 OR(&s->wake_generation,bit);queue_reply(s);
 if(bit==FENCE_ACK)hard_stop(s);
}
static SH int post(struct laz8_msg*m,uint32_t id,void*p,struct live_shutter_z8*s){
 m->id=id;m->param=(uintptr_t)ack;m->payload=p;m->tail=(uintptr_t)s;
 return !FN(0x40c00bac,int(*)(void*,void*))((void*)UINT64_C(0xa02d4c600),m);
}
static SH void event_kick(struct live_shutter_z8*s){
 uint32_t f=LOAD(&s->wake_generation);
 while(!(f&(EVENT_ARMED|FENCE_SENT))){
  if(__atomic_compare_exchange_n(&s->wake_generation,&f,f|EVENT_ARMED,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
   FN(0x40d2bc8c,void(*)(void*,uint32_t))((void*)UINT64_C(0xa02d4c5d0),0x80);return;
  }
 }
}
static SH void finish(uint32_t unused,void*p,uint32_t n){
 (void)unused;(void)p;(void)n;struct live_runtime*r=runtime();if(!r||!LOAD(&r->shutter.phase))return;
 OR(&r->shutter.wake_generation,BARRIER);event_kick(&r->shutter);
}
/* Exact AudioBuffer event-thread wait callsite only. A marker observed in
 * one batch is acknowledged at the NEXT wait, after all that batch's native
 * TX/RX posts. 708 then crosses the lower queue behind every old 70b. This
 * closes the software-producer gap that the native 70e alone cannot close. */
__attribute__((noinline))
uint32_t live_shutter_event_wait(void*flag,uint32_t mask,uint32_t*bits){
 struct live_runtime*r=runtime();struct live_shutter_z8*s=r?&r->shutter:0;
 if(s&&LOAD(&s->phase)){
  if(*bits&0x80u)__atomic_fetch_and(&s->wake_generation,~EVENT_ARMED,__ATOMIC_ACQ_REL);
  uint32_t f=LOAD(&s->wake_generation);
  if((f&(BARRIER|EVENT_SEEN|SETUP_ACK|GO_ACK|FENCE_SENT|EVENT_ARMED))==(BARRIER|EVENT_SEEN|SETUP_ACK|GO_ACK)&&
     __atomic_compare_exchange_n(&s->wake_generation,&f,f|FENCE_SENT,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
   if(!post(&r->shutter_tx.go,0x42040708u,s->params,s)){
    __atomic_fetch_and(&s->wake_generation,~FENCE_SENT,__ATOMIC_RELEASE);s->errors++;
   }
  }
 }
 /* Native wait returns 1 even when its underlying error leaves output intact. */
 *bits=0;
 uint32_t result=FN(0x40d2bd04,uint32_t(*)(void*,uint32_t,uint32_t*))(flag,mask|0x80u,bits);
 if(result&&s&&LOAD(&s->phase)&&(*bits&0x80u)&&(LOAD(&s->wake_generation)&BARRIER))
  OR(&s->wake_generation,EVENT_SEEN);
 return result;
}
static SH void block(uint32_t index,void*p,uint32_t n){
 (void)index;(void)p;(void)n;struct live_runtime*r=runtime();if(!r)return;
 struct live_shutter_z8*s=&r->shutter;uint32_t f=LOAD(&s->wake_generation);
 if(!LOAD(&s->phase)||(f&STOP))return;
 if(!(f&START_SENT)){STORE(&s->wake_busy,1);return; /* repeat<=1: one pending progress is sufficient */}
 FN(0x40e3559c,void(*)(void))();
}
static SH void shutter_done(uint32_t event,void*p){
 struct live_runtime*r=runtime();if(!r||!LOAD(&r->shutter.phase))return;
 struct live_shutter_z8*s=&r->shutter;
 if(!p||word((uintptr_t)p)!=s->tone)return;
 if(event==0x04102004u){s->outer[0]=*(uint64_t*)p;OR(&s->wake_generation,START_REPLY);}
 if(event==0x04102005u){s->outer[1]=*(uint64_t*)p;OR(&s->wake_generation,DONE_REPLY);}
}
/* Original dispatcher has finished using the START envelope, but its original
 * outer callback/free is postponed until real 708/702 acknowledgements. */
static SH void returned(uint32_t event,void*p){
 (void)event;struct live_runtime*r=runtime();if(!r||r->shutter.held!=(uintptr_t)p)return;
 OR(&r->shutter.wake_generation,RETURNED);queue_reply(&r->shutter);
}
uint32_t live_shutter_prepare(void*argument){
 struct live_runtime*r=runtime();
 if(r&&!LOAD(&r->shutdown)&&LOAD(&r->requested)&&!word(UINT64_C(0xa03bd062c))&&live_shutter_mute_get()){
  uintptr_t copy[3];const uintptr_t*p=argument;
  copy[0]=p[0];copy[1]=p[1];copy[2]=p[2];((uint8_t*)copy)[8]=0;
  return FN(0x40f9cccc,uint32_t(*)(void*))(copy);
 }
 return FN(0x40f9cccc,uint32_t(*)(void*))(argument);
}
static __attribute__((noinline)) uintptr_t photo(struct laz8_msg*m){
 if(m->id!=START||m->param!=0x40e32ed8u||m->tail!=(uintptr_t)m||!m->payload||
    ptr((uintptr_t)m->payload+8)!=0x40e3159cu)return 0;
 uintptr_t p=ptr((uintptr_t)m->payload);if(!p)return 0;
 uint32_t t=word(p+12);
 return t<26u&&((UINT32_C(0x03fffc47)>>t)&1u)&&word(p+8)<=1u&&
   !*(uint8_t*)(p+16)&&ptr(p+24)==DONE?p:0;
}
SH uint32_t live_shutter_status(uint32_t*out){
 struct live_runtime*r=runtime();
 if(r&&r->shutter.wake_cancel){*out=r->shutter.wake_cancel==2?1:4;return 0;}
 return FN(0x40c80aac,uint32_t(*)(uint32_t*))(out);
}
__attribute__((noinline)) uint32_t live_shutter_open(uint32_t*p){
 struct live_runtime*r=runtime();if(!r||r->shutter.wake_cancel!=2)return FN(0x40e333a8,uint32_t(*)(void*))(p);
 return p[0]==2&&p[1]==5&&!p[2]&&p[3]==1&&p[4]&&!(p[4]&3);
}
__attribute__((noinline)) uint32_t live_shutter_volume(uint32_t change,uint32_t mode,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t volume){
 struct live_runtime*r=runtime();
 if(!r||r->shutter.wake_cancel!=2)return FN(0x40e33e04,uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))(change,mode,a,b,c,d,e,volume);
 if(mode>1||volume<37||volume>41)return 0;
 /* Apply native D on every START: cached mode does not prove the R63 route.
  * D writes shared R9=300; readback and D/C/C share the original codec lock.
  * Never reset unknown ADC compression/loopback bits to make a tone work. */
 void*lock=(void*)UINT64_C(0xa02d4c8e8);uint16_t reg9=0;uint32_t ok=0;
 FN(0x40d2c7d0,void(*)(void*))(lock);
 if(FN(0x40c7fd08,int(*)(uint32_t,uint16_t*))(9,&reg9)||reg9!=0x300)goto out;
 if(FN(0x40c7f368,uint32_t(*)(uint32_t,uint32_t))(3,mode))goto fault;
 uint32_t level=volume+21;
 if(FN(0x40c7f0bc,uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))(2,0,0,0,level,level))goto fault;
 if(mode&&FN(0x40c7f0bc,uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))(3,0,0,0,level,level))goto fault;
 ok=1;goto out;
 /* A failed native script may have changed its own cached route. Latch the
  * explicit fault instead of trusting that cache on a later START. */
 fault:r->shutter.outer[2]=1;r->shutter.errors++;
 out:FN(0x40d2c794,void(*)(void*))(lock);return ok;
}
SH uint32_t live_shutter_buffer(void*p){
 struct live_runtime*r=runtime();if(!r||r->shutter.wake_cancel!=2)return FN(0x40e33838,uint32_t(*)(void*))(p);
 struct live_shutter_z8*s=&r->shutter;uint32_t*q=(void*)s->params;uintptr_t a=(uintptr_t)p;
 if(LOAD(&s->wake_generation)&SETUP)return 0;
 q[0]=q[1]=0;s->params[1]=ptr(a);q[4]=word(a+8);q[5]=word(a+12);q[6]=word(a+16);
 q[7]=*(uint8_t*)(a+20);q[8]=q[4]/4;q[9]=0;
 if(!s->params[1]||!q[4]||(q[4]&3)||q[5]!=1||q[6]||q[7]>1)return 0;
 OR(&s->wake_generation,SETUP);
 if(post(&s->wake,0x42040708u,q,s))return 1;
 __atomic_fetch_and(&s->wake_generation,~SETUP,__ATOMIC_RELEASE);return 0;
}
SH uint32_t live_shutter_go(void*p){
 struct live_runtime*r=runtime();if(!r||r->shutter.wake_cancel!=2)return FN(0x40e33440,uint32_t(*)(void*))(p);
 struct live_shutter_z8*s=&r->shutter;uint32_t*q=(void*)r->shutter_tx.go_params;
 if(word((uintptr_t)p)!=2||ptr((uintptr_t)p+8)!=0x40e3559cu||(LOAD(&s->wake_generation)&GO))return 0;
 q[0]=0;q[1]=2;q[2]=0;q[3]=0;r->shutter_tx.go_params[2]=(uintptr_t)block;
 r->shutter_tx.go_params[3]=(uintptr_t)finish;q[8]=word((uintptr_t)p+16);q[9]=0;
 codec(0x42040902u,(void*)2);OR(&s->wake_generation,GO);
 if(post(&r->shutter_tx.go,0x42040702u,q,s))return 1;
 __atomic_fetch_and(&s->wake_generation,~GO,__ATOMIC_RELEASE);codec(0x42040903u,(void*)2);return 0;
}
SH uint32_t live_shutter_stop(void){
 struct live_runtime*r=runtime();
 if(!r||!LOAD(&r->shutter.phase)){
  /* A duplicate queued native STOP after our completion must not stop RX. */
  if(r&&ptr(UINT64_C(0xa038feb98))==(uintptr_t)shutter_done&&word(UINT64_C(0xa02d4cb48))==5)return 1;
  return FN(0x40e334d4,uint32_t(*)(void))();
 }
 struct live_shutter_z8*s=&r->shutter;if(stopped(s))return 1;
 if(!(LOAD(&s->wake_generation)&GO))return 0; /* setup ACK only; no TX existed */
 uint32_t f=OR(&s->wake_generation,STOP);
 if(!(f&STOP)){
  codec(0x42040903u,(void*)2);
  if(!post(&r->shutter_tx.stop,0x42040703u,0,s)){
   __atomic_fetch_and(&s->wake_generation,~STOP,__ATOMIC_RELEASE);OR(&s->wake_generation,FAILED);s->errors++;
  }
 }
 return 0;
}
SH uint32_t live_shutter_failure(void){
 struct live_runtime*r=runtime();if(!r||!LOAD(&r->shutter.phase))return FN(0x40e34c0c,uint32_t(*)(void))();
 OR(&r->shutter.wake_generation,FAILED);(void)live_shutter_stop();return 0;
}
SH void live_shutter_cleanup(void){
 if(ptr(UINT64_C(0xa038feb98))!=(uintptr_t)shutter_done)FN(0x40e3574c,void(*)(void))();
}
static SH void replies(struct live_shutter_z8*s){
 uint32_t f=LOAD(&s->wake_generation);
 if((f&START_REPLY)&&!(f&START_SENT)&&(!(f&GO)||(f&GO_ACK))&&(!(f&SETUP)||(f&SETUP_ACK))){
  OR(&s->wake_generation,START_SENT);
  if((f&GO_ACK)&&!(s->outer[0]>>32))s->played++;FN(DONE,reply_fn)(0x04102004u,&s->outer[0]);
  uint32_t n=__atomic_exchange_n(&s->wake_busy,0,__ATOMIC_ACQ_REL);
  while(n--&&!(LOAD(&s->wake_generation)&STOP))FN(0x40e3559c,void(*)(void))();
 }
 f=LOAD(&s->wake_generation);
 if((f&(START_SENT|DONE_REPLY|DONE_SENT))==(START_SENT|DONE_REPLY)&&stopped(s)){
  OR(&s->wake_generation,DONE_SENT);FN(DONE,reply_fn)(0x04102005u,&s->outer[1]);s->completed++;
 }
}
SH void live_shutter_dispatch(void*message){
 struct laz8_msg*m=message;struct live_runtime*r=runtime();
 if(!r||!m){FN(DISPATCH,void(*)(void*))(m);return;}
 struct live_shutter_z8*s=&r->shutter;
 if(m==(void*)s->held&&(LOAD(&s->wake_generation)&RETURNED)){
  s->held=0;replies(s);FN(0x40e32ed8,reply_fn)(0x04102000u,m);
 }else{
  uintptr_t p=photo(m);
  if(p&&wanted(r)){
   s->wake_cancel=!allowed(r);
   if(!s->wake_cancel&&!LOAD(&s->phase)&&!word(UINT64_C(0xa038feb74))&&!ptr(UINT64_C(0xa038febb8))&&
      r->audio_z8.phase==LAZ8_RUNNING&&word(UINT64_C(0xa02d4cb48))==5&&
      (word(UINT64_C(0xa02d4c788))&3u)==2u){
    s->held=(uintptr_t)m;s->tone=word(p+12);s->began=live_audio_clock_us();s->generation++;
    s->wake_generation=s->wake_busy=s->cancel_posted=0;s->captured++;
    s->wake_cancel=2;STORE(&s->phase,1);*(uintptr_t*)(p+24)=(uintptr_t)shutter_done;m->param=(uintptr_t)returned;
   }
  }
  if(m->id==START+2)STORE(&s->cancel_posted,0);
  FN(DISPATCH,void(*)(void*))(m);s->wake_cancel=0;
  if(LOAD(&s->phase)){replies(s);queue_reply(s);}
 }
 if(LOAD(&s->phase)&&!s->held&&(LOAD(&s->wake_generation)&DONE_SENT)&&stopped(s)&&
    !word(UINT64_C(0xa038feb74))&&!ptr(UINT64_C(0xa038febb8)))STORE(&s->phase,0);
}
SH int live_shutter_step(struct live_runtime*r,int want,uint64_t now){
 struct live_shutter_z8*s=&r->shutter;if(!LOAD(&s->phase))return 0;
 uint32_t f=LOAD(&s->wake_generation);
 queue_reply(s);
 if(f&BARRIER)event_kick(s);
 if((!want||!allowed(r)||(f&FAILED)||((f&GO)&&!(f&GO_ACK)&&now-s->began>2000000u))&&
    (!(f&STOP)||stopped(s)))hard_stop(s);
 if((f&STOP)&&stopped(s)&&!(f&DONE_SENT))hard_stop(s);
 /* v71.5 (#221): bounded exit timeout so a stuck native shutter TX can never
  * permanently pin audio.active=1 / 11 MiB audio.descriptor during playback or
  * Wave5 handoff. Latch outer[2]=1 so lower-queue buffers are never reused. */
 if(!want&&now>s->began&&now-s->began>1500000u){
  s->outer[2]=1;s->errors++;OR(&s->wake_generation,FAILED);
  STORE(&s->phase,0);return 0;
 }
 /* Keep polling while TX drains during playback/shutdown. A frozen PCM
  * window remains readable; no cancel, pause, resume or silence bridge. */
 if(!want||r->audio_error){int e=live_audio_z8_poll(&r->audio,&r->audio_z8);
  if(e&&e!=LAUDIO_GAP)r->audio_error=(uint32_t)-e;return 1;}
 return 0;
}
