#ifndef SHUTTER_QUEUE_PROTOTYPE_H
#define SHUTTER_QUEUE_PROTOTYPE_H
/* PRIVATE HOST PROTOTYPE. No native hook is bound to this interface.
 * Capture must be BEFORE native sound-running/pending publication; returning
 * fake queued from40e3133c is NOT a compatible capture adapter. */
#include <stdint.h>
#include <stdatomic.h>
struct sq_request {uint32_t tone,repeats,duration_ms,phase;uint64_t ticket;};
struct sq_ops {
 void *ctx;
 int (*pause)(void*); /* 0=ACK+idle+callback drain, >0 busy, <0 fatal; retains PCM */
 int (*start)(void*,const struct sq_request*); /* 1=owned,0=definitely not queued */
 int (*cancel_sound)(void*); /* 1=request accepted; still requires done */
 int (*resume)(void*); /*0=accepted,>0 busy,<0 fatal; retains PCM */
};
struct sq {
 struct sq_ops ops;struct sq_request slots[2];
 _Atomic uint32_t head,tail,cancel;_Atomic uint64_t active,done;
 uint64_t began;uint32_t state,cancel_posted,overflow,errors;
};
enum {SQ_IDLE,SQ_PAUSE,SQ_PLAY,SQ_RESUME,SQ_RETAINED};
void sq_init(struct sq*,const struct sq_ops*);
int sq_submit(struct sq*,uint32_t tone,uint32_t repeats,uint32_t duration,uint32_t phase,int audible);
void sq_cancel(struct sq*);void sq_done(struct sq*,uint64_t ticket);void sq_step(struct sq*,uint64_t us);
#endif
