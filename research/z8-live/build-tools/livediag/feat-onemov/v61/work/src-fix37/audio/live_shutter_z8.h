#ifndef LIVE_SHUTTER_Z8_H
#define LIVE_SHUTTER_Z8_H
#include "live_audio_z8.h"
/* Runtime-owned, boot-zeroed. Native task owns held/state/tone/generation;
 * Live worker only posts one stable wake or original native hard-stop. */
struct live_shutter_z8 {
 uintptr_t held;
 uint64_t began;
 uint32_t phase,generation,tone,wake_busy,wake_generation,wake_cancel,cancel_posted;
 uint32_t captured,played,completed,cancelled,errors;
 struct laz8_msg wake;
 uintptr_t outer[3],params[5];
};
struct live_runtime;
void live_shutter_dispatch(void *native_message);
/* Returns1 while the shutter owns/pends audio; caller retains PCM and still
 * performs clip mark/freeze. Returns0 when normal audio lifetime may proceed. */
int live_shutter_step(struct live_runtime *,int want,uint64_t now);
#endif
