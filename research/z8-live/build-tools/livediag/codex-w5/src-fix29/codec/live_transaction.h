#ifndef LIVE_TRANSACTION_H
#define LIVE_TRANSACTION_H
#include "live_codec.h"
/* File callbacks return 0 on success. create_exclusive MUST atomically refuse
   existing paths; unsupported native semantics must return failure, not emulate
   with an existence check + truncating open. All handles use one API family. */
typedef struct {
 void *opaque;
 int (*open_read)(void *,const char *,int *);
 int (*create_exclusive)(void *,const char *,int *);
 int (*seek)(void *,int,uint64_t,uint64_t *actual);
 int (*read)(void *,int,void *,size_t,size_t *actual);
 int (*write)(void *,int,const void *,size_t,size_t *actual);
 int (*close)(void *,int);
 int (*remove_owned)(void *,const char *);
 int (*cancelled)(void *);
 void (*yield)(void *);
 /* 0=start, 1=frame committed, 2=end; optional diagnostic callback. */
 void (*encode_event)(void *,unsigned);
 int (*ensure_dir)(void *,const char *);
} live_file_ops;
typedef struct {
 const char *native_path; /* Final path from THIS still's writer ticket. */
 uint64_t native_size,press_us;
 unsigned volume_generation;
 unsigned closed_and_registered; /* Required producer evidence, not a guess. */
 unsigned has_jpeg; /* NEF-only => MOV only. */
 char uuid[36];
 /* Optional in-camera playback preview (PREV.LVP); null skips it. */
 int (*preview_frame)(void *ctx,uint32_t index,void *i420,size_t bytes);
 void *preview_ctx,*preview_scratch;
 /* Optional audio track; null or zero frames skips it. get fills s16 stereo
  * (frames, not samples); trak emits the soun trak for PCM at pcm_offset.
  * Audio failure drops only the audio track, never the clip. */
 int (*audio_get)(void *ctx,uint32_t frame_offset,int16_t *out,uint32_t cap_frames,uint32_t *got);
 size_t (*audio_trak)(void *ctx,uint8_t *out,size_t cap,uint64_t pcm_offset);
 void *audio_ctx;uint32_t audio_frames;
 uint32_t *orient_out; /* v13: receives the still's EXIF orientation before the preview */
} live_still_ticket;
typedef struct { const uint8_t *i420; uint64_t pts_us; } live_input_frame;
/* Flat same-volume layout: LIVE/folder/stem.JPG|.MOV (importable pair) and
   LIVEDATA/folder/stem.MAN|.LVP (camera-only), stem = native DCF stem. */
int live_paths(const char *source, char jpg[128], char mov[128]);
int live_data_paths(const char *source, char man[128], char lvp[128]);
typedef struct { live_encoder encoder;live_mov movie;uint8_t io[65540]; } live_transaction_workspace;
/* persistent/scratch supplied from live_encoder_sizes and 16-byte aligned.
   Return 0 success, -1 I/O/encoding/contract error, -2 cancelled.
   cleanup_failed bit1 JPEG / bit2 MOV / bit4 manifest / bit8 preview means own orphan could not be removed.
   NEVER edits native_path, never allocates, caller owns all lifetime/locks. */
int live_write_pair(live_transaction_workspace *,const live_file_ops *,
 const live_still_ticket *,const live_input_frame *,unsigned count,
 uint32_t last_duration_us,void *persist,void *scratch,unsigned *cleanup_failed);
typedef struct { const uint8_t *annexb; size_t bytes; uint64_t pts_us; } live_input_au;
int live_write_pair_aus(live_transaction_workspace *,const live_file_ops *,
 const live_still_ticket *,const live_input_au *,unsigned count,unsigned width,
 unsigned height,uint32_t last_duration_us,unsigned *cleanup_failed);
#endif
