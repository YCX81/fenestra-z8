#ifndef LIVE_CODEC_H
#define LIVE_CODEC_H
#include <stddef.h>
#include <stdint.h>
#define LIVE_MAX_FRAMES 128
/* The native Wave5 mode entry and Nikon metadata describe the same profile. */
#define LIVE_W5_FPS_NUM 30000u
#define LIVE_W5_FPS_DEN 1001u
/* All functions return 0 on success. Memory buffers must be 16-byte aligned. */
typedef struct { void *persist, *scratch; unsigned width,height,frames; uint64_t last_pts; struct live_w5 *hw; } live_encoder;
#define LIVE_ENCODER_BUSY (-30)   /* Wave5 held by a stock user: drop the frame, retry later */
int live_encoder_sizes(unsigned w,unsigned h,size_t *persist,size_t *scratch);
void live_encoder_set_flags(live_encoder *,unsigned);
void live_encoder_preempt(live_encoder *,uint32_t client);
int live_encoder_release(live_encoder *); /* v27 */
/* Read-only, under stock_handoff_lock after worker quiescence. */
int live_encoder_retained(const live_encoder *);
/* Stock caller, only after the worker's complete quiescence ACK. Refuses a
 * live/busy/failed-close instance; success also releases persistent pool memory. */
int live_encoder_dispose(live_encoder *);
int live_encoder_codec(void); /* 0 AVC, 1 HEVC: what the encoder emits, for the MOV sample entry */
int live_encoder_init(live_encoder *,void *persist,void *scratch,unsigned w,unsigned h);
int live_encoder_frame(live_encoder *,const uint8_t *i420,const uint8_t **annexb,size_t *bytes);
int live_encoder_frame_timed(live_encoder *,const uint8_t *,uint64_t pts_us,
 uint64_t *last_idr_us,unsigned bitrate,unsigned idr_interval_us,
 const uint8_t **annexb,size_t *bytes,unsigned *idr);
/* write_at must finish the entire write or return nonzero; no allocation or file APIs here. */
typedef int (*live_write_at)(void *,uint64_t,const void *,size_t);
typedef enum { LIVE_CODEC_AVC, LIVE_CODEC_HEVC } live_codec_type;
typedef struct {
 live_write_at write; void *opaque; uint32_t pos,count,width,height;
 uint32_t offsets[LIVE_MAX_FRAMES],sizes[LIVE_MAX_FRAMES],duration[LIVE_MAX_FRAMES];
 uint64_t pts[LIVE_MAX_FRAMES]; uint8_t sync[LIVE_MAX_FRAMES];
 uint8_t sps[1024],pps[1024]; unsigned sps_n,pps_n; int failed;   /* v43: hw HEVC SPS/PPS > 256 B */
 uint8_t atoms[16384];
 unsigned orient; /* v13: EXIF orientation of the still, 1 = none */
 live_codec_type codec;
 uint8_t vps[256],hevc_config[23];
 unsigned vps_n;
 uint32_t moov_at; /* v50: append streamed Nikon udta before the trailing preview. */
 uint32_t meta_at,meta_bytes; /* pending small metadata tail within atoms, until extend_moov */
} live_mov;
int live_mov_begin_codec(live_mov *,live_write_at,void *,unsigned w,unsigned h,live_codec_type);
int live_mov_begin(live_mov *,live_write_at,void *,unsigned w,unsigned h);
int live_mov_add(live_mov *,const uint8_t *annexb,size_t bytes,uint64_t pts_us);
/* press_us must lie in the actual frame window; last_duration_us is measured/estimated explicitly. */
int live_mov_finish(live_mov *,uint64_t press_us,uint32_t last_duration_us,const char uuid[36]);
/* Raw bytes into mdat after the video samples (e.g. PCM); *at receives the file
   offset of the first byte. Call before finish. */
int live_mov_append_raw(live_mov *,const void *,size_t,uint64_t *at);
/* Extend the already finished moov after caller streamed a complete udta. */
int live_mov_extend_moov(live_mov *,uint64_t end);
/* Optional extra trak (track ID 3) written into moov; returns bytes or 0 on error. */
typedef size_t (*live_mov_trak_fn)(void *ctx,uint8_t *out,size_t cap);
int live_mov_finish_with(live_mov *,uint64_t press_us,uint32_t last_duration_us,
 const char uuid[36],live_mov_trak_fn,void *ctx);
/* Transaction-only: write moov prefix; stream udta then call extend_moov,
 * which appends the preserved Apple metadata tail as the last moov children. */
int live_mov_finish_prefix(live_mov *,uint64_t press_us,uint32_t last_duration_us,
 const char uuid[36],live_mov_trak_fn,void *ctx);
/* In-place APP1 prefix edit on a COPY only. Full file offsets/length remain identical.
   Supply prefix through end of EXIF APP1 (normally <=65540). Rejects missing/short MakerNote. */
int live_jpeg_pair_prefix(uint8_t *prefix,size_t bytes,const char uuid[36]);
/* v13: EXIF IFD0 Orientation (1..8) of a JPEG prefix, 1 when absent or malformed. */
unsigned live_jpeg_orientation(const uint8_t *prefix,size_t bytes);
#endif
