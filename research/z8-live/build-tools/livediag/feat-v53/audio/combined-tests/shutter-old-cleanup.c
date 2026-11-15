#include <stdint.h>
extern void *test_function(uintptr_t);
extern uint32_t test_word(uintptr_t);
extern uintptr_t test_ptr(uintptr_t);
extern uint8_t test_byte(uintptr_t);
#include "../runtime.h"
#include "../ui/live_ui.h"
#define FN(a,t) ((t)test_function((uintptr_t)(a)))
#define LOAD(p) __atomic_load_n(p,__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
#define SH __attribute__((noinline))
#define NATIVE_DISPATCH 0x40e34b2cu
#define NATIVE_DONE 0x40f9d194u
#define START 0x04100030u
enum { SH_IDLE,SH_HELD,SH_NATIVE,SH_DONE,SH_RESUMING };
typedef void (*reply_fn)(uint32_t,void *);
static uint32_t word(uintptr_t a){return test_word(a);}
static uintptr_t ptr(uintptr_t a){return test_ptr(a);}
static SH struct live_runtime *runtime(void){
 struct live_runtime *r=live_ui_engine_get();
 return r&&r->magic==LIVE_RUNTIME_MAGIC&&r->size==sizeof *r?r:0;
}
static SH int allowed(struct live_runtime*r){
 return r&&!LOAD(&r->shutdown)&&LOAD(&r->requested)&&!LOAD(&r->crumb_pad)&&
  word(UINT64_C(0xa03bd062c))==0&&!live_shutter_mute_get()&&
  !test_byte(UINT64_C(0xa03bc7008))&&
  r->audio.active&&!LOAD(&r->audio.cancelled)&&!r->audio.frozen;
}
static SH int sound_idle(void){return !word(UINT64_C(0xa038feb74))&&!ptr(UINT64_C(0xa038febb8));}
static uintptr_t params(struct laz8_msg*m){return m->payload?ptr((uintptr_t)m->payload):0;}
static SH int normal(struct laz8_msg*m){
 uintptr_t p=params(m);
 return m->id==START&&m->param==0x40e32ed8u&&m->tail==(uintptr_t)m&&p&&
  ptr((uintptr_t)m->payload+8)==0x40e3159cu&&
  (word(p+12)==1||word(p+12)==2)&&word(p+8)==1&&ptr(p+24)==NATIVE_DONE;
}
static SH int cancel_shape(struct laz8_msg*m){
 return m->payload&&m->param==0x40e32ed8u&&m->tail==(uintptr_t)m&&
  ptr((uintptr_t)m->payload+8)==0x40e3159cu&&!params(m);
}
static void outer(struct laz8_msg*m,uint32_t event,uint32_t status){
 *(uint32_t *)((uintptr_t)m->payload+16)=status;
 ((reply_fn)(uintptr_t)m->param)(event,(void *)(uintptr_t)m->tail);
}
/* Called only by original serialized SFX task; no original queue owns held.
 * Reproduce the complete native driver5 rejection, including heap cleanup. */
static SH void reject_held(struct live_shutter_z8*s){
 struct laz8_msg*m=(void *)s->held;uintptr_t p=params(m);
 uint32_t result[2]={word(p+12),0};reply_fn cb=(reply_fn)ptr(p+24);
 s->held=0;cb(0x04102004u,result);cb(0x04102005u,result);
 outer(m,0x04102000u,1);s->cancelled++;STORE(&s->phase,SH_DONE);
}
static SH void shutter_done(uint32_t event,void*payload){
 FN(NATIVE_DONE,reply_fn)(event,payload);
 struct live_runtime*r=runtime();if(!r)return;
 struct live_shutter_z8*s=&r->shutter;
 if(event==0x04102005u&&LOAD(&s->phase)==SH_NATIVE&&payload&&word((uintptr_t)payload)==s->tone){
  s->completed++;STORE(&s->phase,SH_DONE);
 }
}
SH void live_shutter_dispatch(void *message){
 struct laz8_msg*m=message;struct live_runtime*r=runtime();
 if(!r||!m){FN(NATIVE_DISPATCH,void(*)(void*))(m);return;}
 struct live_shutter_z8*s=&r->shutter;
 if(m==&s->wake){
  uint32_t generation=LOAD(&s->wake_generation);
  if(generation==LOAD(&s->generation)&&LOAD(&s->phase)==SH_NATIVE&&s->wake_cancel&&
     ptr(UINT64_C(0xa038feb98))==(uintptr_t)shutter_done&&!s->cancel_posted){
   /* Same task owns native completion/cancel ordering. Posting only requests
    * STOP; it is never interpreted as hardware completion. */
   if(FN(0x40e316bc,int(*)(void))())s->cancel_posted=1;
  }
  if(LOAD(&s->phase)==SH_HELD&&generation==LOAD(&s->generation)){
   if(s->wake_cancel||!allowed(r)){if(sound_idle())reject_held(s);else s->errors++;}
   else if(r->audio_z8.phase==LAZ8_IDLE&&word(UINT64_C(0xa02d4cb48))==1&&sound_idle()){
    struct laz8_msg*held=(void *)s->held;uintptr_t p=params(held);
    s->held=0;*(uintptr_t *)(p+24)=(uintptr_t)shutter_done;
    s->played++;STORE(&s->phase,SH_NATIVE);
    FN(NATIVE_DISPATCH,void(*)(void*))(held);
   }
  }
  STORE(&s->wake_busy,0);return;
 }
 if(LOAD(&s->phase)==SH_HELD&&(m->id==START||m->id==START+1||m->id==START+2)){
  /* Existing native task may cancel prepare before fire. Preserve that
   * ordering, never delay exposure to insist both tones finish. */
  if(!sound_idle()){s->errors++;FN(NATIVE_DISPATCH,void(*)(void*))(m);return;}
  reject_held(s);
  if(m->id!=START&&cancel_shape(m)){outer(m,m->id+0x1fd0u,0);return;}
 }
 uint32_t phase=LOAD(&s->phase);
 if((phase==SH_IDLE||phase==SH_DONE||phase==SH_RESUMING)&&normal(m)&&allowed(r)&&sound_idle()&&
    (r->audio_z8.phase!=LAZ8_IDLE||phase!=SH_IDLE)){
  uint32_t generation=s->generation+1u;if(generation){
   s->held=(uintptr_t)m;s->tone=word(params(m)+12);s->generation=generation;
   s->began=live_audio_clock_us();s->cancel_posted=0;s->captured++;STORE(&s->phase,SH_HELD);return;
  }
 }
 FN(NATIVE_DISPATCH,void(*)(void*))(m);
}
static SH void notify_ready(struct live_shutter_z8*s,int cancel){
 uint32_t generation=LOAD(&s->generation),zero=0;
 if(!__atomic_compare_exchange_n(&s->wake_busy,&zero,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
 /* Stable inert030 fallback: if ever passed to unhooked native dispatch it
  * is a valid unsupported tone60 with no callback, and owns no heap. */
 s->outer[0]=(uintptr_t)s->params;s->outer[1]=0;s->outer[2]=0;
 s->params[0]=0;s->params[1]=UINT64_C(60)<<32|1u;s->params[2]=s->params[3]=s->params[4]=0;
 s->wake.id=START;s->wake.payload=s->outer;s->wake.param=0;s->wake.tail=0;
 s->wake_cancel=(uint32_t)cancel;STORE(&s->wake_generation,generation);
 if(FN(0x40c00bac,int(*)(void*,void*))((void *)ptr(UINT64_C(0xa038fea70)),&s->wake)!=0){
  STORE(&s->wake_busy,0);s->errors++;
 }
}
SH int live_shutter_step(struct live_runtime*r,int want,uint64_t now){
 struct live_shutter_z8*s=&r->shutter;uint32_t phase=LOAD(&s->phase);
 if(phase==SH_IDLE)return 0;
 if(phase==SH_HELD){
  if(!want||!allowed(r)||(now>=s->began&&now-s->began>2000000u)){
   notify_ready(s,1);return 1;
  }
  int e=live_audio_z8_pause(&r->audio,&r->audio_z8);
  if(!e)notify_ready(s,0);else if(e!=LAUDIO_BUSY){
   s->errors++;STORE(&r->audio_warm,1u);r->audio_error=(uint32_t)-e;
   live_audio_cancel(&r->audio);notify_ready(s,1);
  }
  return 1;
 }
 if(phase==SH_NATIVE){
  if((!want||!allowed(r)||(now>=s->began&&now-s->began>2000000u)||
      (!word(UINT64_C(0xa038feb74))&&!test_byte(UINT64_C(0xa038fe9f1))))&&
     ptr(UINT64_C(0xa038feb98))==(uintptr_t)shutter_done&&!s->cancel_posted){
   notify_ready(s,1);
  }
  return 1;
 }
 /* Software cancellation may beat the worker before any mic pause. */
 if(r->audio_z8.phase==LAZ8_RUNNING){
  uint32_t done=SH_DONE;
  return !__atomic_compare_exchange_n(&s->phase,&done,SH_IDLE,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
 }
 if(r->audio_z8.phase==LAZ8_STOPPING){
  if(live_audio_z8_pause(&r->audio,&r->audio_z8))return 1;
 }
 if(want&&r->audio.frozen)return 1; /* caller still thaws after mux */
 /* Accepted2004 is not done. Even2005 precedes the original outer cleanup;
  * wait for both real driver idle and the native pending flag to clear. */
 if(!sound_idle()||word(UINT64_C(0xa02d4cb48))!=1||
    test_byte(UINT64_C(0xa038fe9f1)))return 1;
 uint32_t done=SH_DONE;
 if(!__atomic_compare_exchange_n(&s->phase,&done,SH_RESUMING,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 1;
 if(want&&allowed(r)){
  if(live_audio_z8_resume(&r->audio,&r->audio_z8)){s->errors++;live_audio_cancel(&r->audio);}
 }
 uint32_t resuming=SH_RESUMING;
 /* A new START may already have been retained while GO was posting. Never
  * overwrite that new owner; its next step will stop the fresh session. */
 return !__atomic_compare_exchange_n(&s->phase,&resuming,SH_IDLE,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
