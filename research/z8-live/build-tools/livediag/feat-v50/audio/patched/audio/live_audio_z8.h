#ifndef LIVE_AUDIO_Z8_H
#define LIVE_AUDIO_Z8_H
#include "live_audio.h"
/* Z8 C3.11 photo-mode capture backend, hardware-checked 2026-11-04 through the
 * stock SoundTest path (audiodbg 1) plus WM8962 R21/R22 un-mute. It replays the
 * SoundTest audio-driver messages with our own callback and no 23 MB buffer:
 * 0x42040104 format, 0x4204010b channels, 0x42040101, 0x42040102 mode,
 * 0x42040109 ring+callback, 0x42040106 go; stop is 0x42040108.
 * DMA ring: stock fixed audio buffer (descriptor 0xa031f8bb8), 5 blocks of
 * 24000 s16 stereo frames (0x17700 B, 0.5 s). */
#define LAZ8_BLOCK_FRAMES 24000u
#define LAZ8_BLOCK_BYTES (LAZ8_BLOCK_FRAMES * 4u)
#define LAZ8_SLOTS 4u
#define LAZ8_STAGE_BYTES (LAZ8_SLOTS * LAZ8_BLOCK_BYTES)
#define LAZ8_ADC_0DB 0x1c0u /* 0xC0 = 0 dB, bit 8 = volume update */
enum { LAZ8_IDLE, LAZ8_STARTING, LAZ8_RUNNING, LAZ8_STOPPING };
struct laz8_msg { /* 0x38-byte stock message; post fills +0x10 and the kernel +0..+0xf */
 uint64_t link[3], param;
 uint32_t id, pad;
 void *payload;
 uint64_t tail;
};
/* Native entry points; live_audio_z8_init fills the C3.11 addresses, tests replace them. */
struct laz8_native {
 int (*post)(struct laz8_msg *);                         /* 0x40c80988, bit0 = queued */
 int (*codec_read)(uint32_t reg,uint16_t *value);        /* 0x40c7fd08, 0 = ok */
 int (*codec_write)(uint32_t reg,uint32_t value);        /* 0x40c7f824, 0 = ok */
 void (*sleep)(uint32_t ms);                             /* 0x40187b00 */
 const volatile uint32_t *service_state;                 /* 0xa02d4cb48: 1 idle, 5 capturing */
 const volatile uint64_t *ring_descriptor;               /* 0xa031f8bb8: [0] = DMA ring base */
 int (*allocate)(void *,uint32_t,void **,uint8_t **,uint32_t *);
 void (*release)(void *,void *);
 uint64_t (*clock_us)(void);
};
struct live_audio_z8 {
 struct laz8_native native;
 struct laz8_msg msg[7];
 uint8_t format[0x20],channels[8],route[0x10],mode[0x10],go[8],stop_payload[8];
 struct {
  uint32_t ring_count,block_bytes,zero,block_frames;
  uint8_t enable,pad[7];
  uint64_t ring,callback,tail;
 } ring;
 uint8_t *stage;
 uint64_t stamp_us[LAZ8_SLOTS];
 uint32_t head,tail,overrun,closed,phase,unmuted,saved_valid,adc_volume;
 uint16_t saved[2];
 uint64_t anchor_us,frames;
 uint32_t blocks,dropped,stop_timeouts,codec_errors;
};
/* Integrator provides the instance the DMA callback should feed (e.g. a field
 * of the Live runtime found through its registry); 0 makes the callback a no-op. */
struct live_audio_z8 *live_audio_z8_context(void);
void live_audio_z8_init(struct live_audio_z8 *,uint32_t adc_volume);
void live_audio_z8_ops(struct live_audio_z8 *,struct live_audio_ops *);
/* Audio-driver callback (stock SoundTest signature): copies one block, no push. */
void live_audio_z8_block(const void *pcm,uint32_t bytes);
/* Worker-only: un-mutes after the first block, then pushes staged blocks.
 * Returns LAUDIO_OK, LAUDIO_GAP after an overrun (history restarts), or the
 * live_audio_push error. */
int live_audio_z8_poll(struct live_audio *,struct live_audio_z8 *);
#endif
