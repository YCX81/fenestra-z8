#ifndef LIVE_AUDIO_H
#define LIVE_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#define LIVE_AUDIO_RATE 48000u
#define LIVE_AUDIO_CHANNELS 2u
#define LIVE_AUDIO_CAPACITY (4u * LIVE_AUDIO_RATE)
#define LIVE_AUDIO_BYTES (LIVE_AUDIO_CAPACITY * 4u)
enum live_audio_result { LAUDIO_OK=0, LAUDIO_UNAVAILABLE=-1, LAUDIO_ALLOC=-2,
 LAUDIO_STATE=-3, LAUDIO_RANGE=-4, LAUDIO_GAP=-5, LAUDIO_CANCELLED=-6 };
enum live_audio_input { LAUDIO_S16LE=2, LAUDIO_S24LE=3, LAUDIO_S24_IN_S32LE=4 };
/* ALL stateful calls run in one serialized task, except cancel (atomic).
 * IRQ only records a completion + timestamp for that task. Never feed in IRQ.
 * Backend guarantees CPU-visible immutable PCM until push returns; it promptly
 * returns native DMA buffers. Input is stereo interleaved, L then R.
 * S24_IN_S32LE means signed 24-bit LEFT-aligned in 32 bits, NOT right-aligned.
 * This packing is an explicit input contract, not a claim about Z8 silicon. */
struct live_audio_ops {
 void *opaque;
 int (*allocate)(void *,uint32_t,void **descriptor,uint8_t **cpu,uint32_t *actual);
 void (*release)(void *,void *descriptor);
 int (*start_capture)(void *); /* 0 success; failure must undo partial setup */
 void (*stop_capture)(void *); /* synchronous quiesce; no DMA into our ring */
 int verified_photo_backend; /* zero for the supplied C3.11 adapter */
};
struct live_audio_window { uint64_t begin_us,end_us; }; /* same window as video */
struct live_audio_duck {
 uint32_t before_us,after_us,ramp_us,gain_q15; /* 0 mute; 32768 unity */
};
struct live_audio_info {
 uint32_t rate,channels,bits,frames;
 uint64_t first_sample_us,window_begin_us,window_end_us;
};
struct live_audio {
 struct live_audio_ops ops;
 void *descriptor;
 int16_t *ring;
 uint64_t first_index,next_index,press_us,exposure_us,freeze_first;
 uint32_t count,freeze_frames,active,frozen,marked,cancelled;
 struct live_audio_window window;
 struct live_audio_duck duck;
};
void live_audio_init(struct live_audio *,const struct live_audio_ops *);
int live_audio_start(struct live_audio *);
void live_audio_cancel(struct live_audio *); /* mode/sleep/card/off IRQ-safe flag */
void live_audio_stop(struct live_audio *); /* worker after cancel/at toggle off */
int live_audio_mark(struct live_audio *,uint64_t press_us);
/* Optional actual exposure anchor; mark defaults duck center to full press. */
int live_audio_exposure(struct live_audio *,uint64_t exposure_us);
int live_audio_set_duck(struct live_audio *,struct live_audio_duck);
/* Timestamp is the FIRST SAMPLE time in the shared physical-counter epoch,
 * not task wake time or block completion time. Block sizes <= capacity.
 * Reject gaps/overlap > 1 sample. See INTEGRATION.md for DMA clock calibration. */
int live_audio_push(struct live_audio *,const void *,uint32_t frames,
                    enum live_audio_input,uint64_t first_sample_us);
int live_audio_freeze(struct live_audio *,struct live_audio_window);
int live_audio_get(struct live_audio *,uint32_t frame_offset,int16_t *stereo,
                   uint32_t capacity_frames,uint32_t *got,struct live_audio_info *);
void live_audio_thaw(struct live_audio *); /* drop history after writer finishes */
uint64_t live_audio_clock_us(void); /* CNTPCT_EL0/CNTFRQ_EL0, same as video */
void live_audio_native_ops(struct live_audio_ops *); /* default unavailable */
int live_audio_native_alloc(void *,uint32_t,void **,uint8_t **,uint32_t *);
void live_audio_native_free(void *,void *);
/* Emit a complete trak, single PCM chunk already written to mdat. Returns
 * bytes or 0 on invalid input / buffer too small. No file I/O, no allocation.
 * movie_timescale must match mvhd; use a unique track_id (normally 3).
 * info.window_begin_us must equal video's movie epoch. */
size_t live_audio_mov_trak(uint8_t *,size_t,const struct live_audio_info *,
                          uint32_t track_id,uint32_t movie_timescale,uint64_t pcm_offset);
#endif
