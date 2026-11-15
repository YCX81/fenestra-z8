#include "held_queue.h"
#define LOAD(p) __atomic_load_n(p,__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n(p,v,__ATOMIC_RELEASE)
int held_queue_hold(struct held_queue *q,void *start){
 uint32_t s=LOAD(&q->held.state);if((s!=0&&s!=3)||!start)return 0;
 uint32_t g=LOAD(&q->held.generation)+1u;if(!g)return 0; /* don't wrap tickets */
 q->held.message=(uintptr_t)start;STORE(&q->held.generation,g);STORE(&q->held.state,1);return 1;
}
int held_queue_notice(struct held_queue *q){
 uint32_t g=LOAD(&q->held.generation);if(LOAD(&q->held.state)!=1)return 0;
 uint32_t zero=0;if(!__atomic_compare_exchange_n(&q->wake_busy,&zero,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
 STORE(&q->wake_generation,g);return 1;
}
void held_queue_post_failed(struct held_queue *q){STORE(&q->wake_busy,0);}
uintptr_t held_queue_take(struct held_queue *q,int may_start){
 uintptr_t m=0;uint32_t g=LOAD(&q->wake_generation);
 if(LOAD(&q->wake_busy)&&may_start&&LOAD(&q->held.state)==1&&g==LOAD(&q->held.generation)){
  /* Native task serializes this against cancel. Never transfer from worker. */
  STORE(&q->held.state,4);m=(uintptr_t)q->held.message;q->held.message=0;
 }
 STORE(&q->wake_busy,0);return m;
}
int held_queue_cancel(struct held_queue *q,void *cancel){
 if(LOAD(&q->held.state)!=1)return 0; /* transferred => original native cancel */
 return q->cancel_before_transfer(&q->held,cancel);
}
