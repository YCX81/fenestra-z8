#include "queue_prototype.h"
#include <string.h>
#define LOAD(p) atomic_load_explicit(p,memory_order_acquire)
#define STORE(p,v) atomic_store_explicit(p,v,memory_order_release)
void sq_init(struct sq*q,const struct sq_ops*ops){memset(q,0,sizeof *q);q->ops=*ops;}
int sq_submit(struct sq*q,uint32_t tone,uint32_t repeats,uint32_t duration,uint32_t phase,int audible){
 if(!audible||LOAD(&q->cancel))return 0;
 /* Only the observed normal pair; looping/bulb/repeated semantics unresolved. */
 if((tone!=1u&&tone!=2u)||repeats!=1u||duration>1000u||phase>1u)return 0;
 uint32_t h=LOAD(&q->head),t=LOAD(&q->tail);
 if(h-t>=2){q->overflow++;return 0;}
 q->slots[h%2]=(struct sq_request){tone,repeats,duration,phase,(uint64_t)h+1u};
 STORE(&q->head,h+1u);return 1;
}
void sq_cancel(struct sq*q){STORE(&q->cancel,1);}
void sq_done(struct sq*q,uint64_t ticket){if(ticket&&ticket==LOAD(&q->active))STORE(&q->done,ticket);}
void sq_step(struct sq*q,uint64_t us){
 if(q->state==SQ_RETAINED)return;
 uint32_t h=LOAD(&q->head),t=LOAD(&q->tail);
 if(q->state==SQ_IDLE){
  if(LOAD(&q->cancel)){STORE(&q->tail,h);return;}
  if(h==t)return;q->state=SQ_PAUSE;q->began=us;
 }
 if(q->state==SQ_PAUSE){
  int r=q->ops.pause(q->ops.ctx);
  if(r>0){if(us-q->began>2000000u){q->errors++;q->state=SQ_RETAINED;}return;}
  if(r<0){q->errors++;q->state=SQ_RETAINED;return;}
  if(LOAD(&q->cancel)){STORE(&q->tail,LOAD(&q->head));q->state=SQ_RESUME;}
  else {
   struct sq_request *job=&q->slots[t%2];STORE(&q->done,0);STORE(&q->active,job->ticket);
   q->began=us;q->cancel_posted=0;
   if(q->ops.start(q->ops.ctx,job)){q->state=SQ_PLAY;return;}
   STORE(&q->active,0);STORE(&q->tail,t+1);q->state=SQ_RESUME;
  }
 }
 if(q->state==SQ_PLAY){
  uint64_t active=LOAD(&q->active);
  if(LOAD(&q->done)==active){
   STORE(&q->active,0);STORE(&q->tail,t+1);
   if(LOAD(&q->cancel))STORE(&q->tail,LOAD(&q->head));
   q->state=SQ_RESUME;
  } else {
   if(LOAD(&q->cancel)&&!q->cancel_posted)q->cancel_posted=!!q->ops.cancel_sound(q->ops.ctx);
   if(us-q->began>2000000u){q->errors++;q->state=SQ_RETAINED;}
   return;
  }
 }
 if(q->state==SQ_RESUME){
  if(LOAD(&q->cancel)){q->state=SQ_IDLE;return;} /* handoff: never re-open capture */
  if(!LOAD(&q->cancel)&&LOAD(&q->head)!=LOAD(&q->tail)){q->state=SQ_PAUSE;q->began=us;return;}
  int r=q->ops.resume(q->ops.ctx);if(r>0)return;
  if(r<0){q->errors++;q->state=SQ_RETAINED;return;}q->state=SQ_IDLE;
 }
}
