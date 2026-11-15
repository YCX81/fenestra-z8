#ifndef HELD_QUEUE_PRIVATE_H
#define HELD_QUEUE_PRIVATE_H
#include "held_sound.h"
/* Isolated protocol component. Admission must be validated by the future
 * original-dispatcher hook; this does not itself intercept messages or stop
 * the microphone. The stable wake envelope remains owned until wake_busy0. */
struct held_queue {
 struct held_sound held;
 int (*cancel_before_transfer)(struct held_sound *,void *);
 uint32_t wake_busy,wake_generation;
};
int held_queue_hold(struct held_queue *,void *validated_start); /* SFX task */
int held_queue_notice(struct held_queue *); /* Live worker:1=>post stable wake */
void held_queue_post_failed(struct held_queue *); /* only definitely not queued */
uintptr_t held_queue_take(struct held_queue *,int may_start); /* SFX wake */
int held_queue_cancel(struct held_queue *,void *cancel); /* SFX task */
#endif
