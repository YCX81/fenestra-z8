#ifndef LIVE_SHUTTER_Z8_H
#define LIVE_SHUTTER_Z8_H
#include "live_audio_z8.h"
/* Runtime-owned, boot-zeroed. Old field offsets stay fixed. wake is lower
 * 708 setup; params its payload; outer holds pending native replies. */
struct live_shutter_z8 {
 uintptr_t held;
 uint64_t began;
 uint32_t phase,generation,tone,wake_busy,wake_generation,wake_cancel,cancel_posted;
 uint32_t captured,played,completed,cancelled,errors;
 struct laz8_msg wake;
 uintptr_t outer[3],params[5];
};
/* Independent DMA queue ownership: stable until every ACK and the final
 * lower-task 70e barrier. No timed reuse, and no high-driver ring change. */
struct live_shutter_tx {
 struct laz8_msg go,stop;
 uintptr_t go_params[5];
};
struct live_runtime;
void live_shutter_dispatch(void *native_message);
/* Normal operation returns 0 so RX poll remains continuous. During an exit
 * with in-flight TX, polls RX itself and returns 1 until actual TX completion;
 * caller retains PCM and still performs clip mark/freeze. */
int live_shutter_step(struct live_runtime *,int want,uint64_t now);
#endif
