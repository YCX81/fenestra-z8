#ifndef LIVE_STREAM_H
#define LIVE_STREAM_H
#include "live_capture.h"
#include "../codec/au_ring.h"
#include "../codec/live_transaction.h"
#ifndef LIVE_BITRATE
#define LIVE_BITRATE 16000000u
#endif
#ifndef LIVE_IDR_US
#define LIVE_IDR_US 500000u
#endif
#define LIVE_BITSTREAM_BYTES (8u*1024u*1024u)
/* I420 preview frames kept alongside the bitstream for in-camera playback.
 * v37 stores them as JPEG pictures inside the Live MOV (codec/preview_writer.h),
 * not as a separate LVP file. Frozen with the clip: no pushes during mux. */
/* v46: a live-view gap longer than this (menu, playback, monitor off) restarts the pre-roll:
 * a clip never splices frames from before the gap. */
#define LIVE_RESUME_GAP_US 1000000u
/* v46: pre-roll that already allows a (shorter) clip */
#define LIVE_WARM_US 500000u
#define LIVE_PREVIEW_SLOTS 50u   /* 3.3 s at 15 fps, about 52 MB */
#define LIVE_PREVIEW_W 1024u
#define LIVE_PREVIEW_H 682u
#define LIVE_PREVIEW_FRAME (LIVE_PREVIEW_W*LIVE_PREVIEW_H*3u/2u)
#define LIVE_PREVIEW_STEP_US 66667u
/* Latest-frame mailbox has capacity one: there is never an unbounded raw queue.
 * Frozen AUs own a distinct byte arena until the mux consumer acknowledges. */
struct live_diag {
 uint32_t magic,version;
 uint64_t steps,ineligible,new_seq,stale,interval,lease_fail,geometry,valid1_fail,convert_fail,
  valid2_fail,seq_changed,cancelled,busy,ok,enc_error;
 uint64_t last_age_us,max_age_us,last_lease_us,max_lease_us,last_convert_us,max_convert_us,sum_convert_us,n_convert;
 uint64_t last_frames_during_copy,max_frames_during_copy;
 uint64_t y,uv;uint32_t width,height,y_stride,uv_stride;
 uint64_t loops,last_loop_us,max_loop_us,last_ui_us,max_ui_us,last_step_us,max_step_us,last_audio_us,max_audio_us,
  last_sleep_us,max_sleep_us,loop_t0;
 uint64_t snap_y_after,snap_uv_after;
 uint64_t bufs[8];uint32_t nbufs,version2;uint64_t accepted_late,fast_rows,slow_rows;
 uint64_t alloc_retry,short_start,geometry_skip,geometry_since;
 uint64_t soft_cancel;
};
struct live_tune {
 uint32_t magic,version;
 int32_t late_max;          /* frames allowed to complete during a copy; -1 = buffers-2 (auto), 0 = strict */
 uint32_t soft_cancel;      /* 1: a cancel while Live stays on drops only the frame in flight */
 uint32_t window_us;        /* clip pre/post window */
 uint32_t sample_us;        /* minimum spacing of encoded frames */
 uint32_t stale_us;         /* frames older than this are skipped */
 uint32_t short_start;      /* 1: no IDR window_us back -> earliest IDR before the press */
 uint32_t geometry_reset_us;/* lasting geometry change without a clip -> restart */
 uint32_t audio;            /* 0: skip the photo-mode audio step */
 uint32_t ready_fresh_us;   /* READY needs an encoded frame at most this old */
 uint32_t cached_lv,cached_work,cached_slot,cached_bs; /* cacheable-alias switches, default 1 */
 uint32_t picker;           /* 0/1 diagnostic groups; 2/3 temporal predecessor (3 default) */
 uint32_t icon_refresh_ms;  /* re-present the Live indicator this often (v37 default 400), 0 = only on change */
};
struct live_ev {uint64_t t;uint32_t type,a;uint64_t b;};
/* v9 flight record, 64 bytes; result codes in flight_patch.py / livectl.py */
struct live_fr {uint64_t t,seq,after,y,s4,s20;uint32_t conv_us,enc_us;uint16_t torn_y,torn_uv;uint8_t result,picker,nbufs,flags;};
#define LIVE_FR 4096u
#define LIVE_LOG 128u
enum {EV_PRESS=1,EV_ACCEPT,EV_SHORT,EV_IGNORED,EV_SOFT_CANCEL,EV_RELEASE,EV_C0,EV_POOL,EV_GEOMETRY,EV_ALLOC,
 EV_FREEZE,EV_FREEZE_FAIL,EV_MUX_BEGIN,EV_MUX_END,EV_WRITER,EV_READY,EV_ENC_ERR,EV_MUX_ABORT,EV_NOTREADY,EV_ICON,EV_MUX_STAGE,EV_DCIM,EV_UI,EV_FLAG,EV_MODE,EV_W5,EV_LOGSAVE,EV_VIEW,EV_PB,
 /* 30 = EV_AUDIO, 31 = shutter, 32 = EV_DIAG1, 33 = EV_DIAG2, 34 = EV_DIAG3 */
 EV_DIAG1=32,EV_DIAG2=33,EV_DIAG3=34,
 EV_RESUME=40,EV_WARM,EV_MUX_PAUSE};
/* V70 Silky30fps: Flip-observed 6-buffer completed-frame history ring.
 * Lives in cached struct live_runtime::lv_regs[228] bytes 256..767 (512 B partition),
 * preserving sizeof(struct live_stream) and sizeof(struct live_runtime) == 23904. */
#define LIVE_FLIP_RING_CAP      6u
#define LIVE_FLIP_FLAG_CONSUMED 1u
#define LIVE_FLIP_FLAG_INTERP   2u
#define LIVE_FLIP_FLAG_BOOT     4u
#define LIVE_CFR_REPEAT_GRACE_US 28000u
#define LIVE_FILMSIM_CPU2_GRACE_US 22000u
#define LIVE_FLIP_PENDING_MAX_US 12000u
#define LIVE_FLIP_REPOLL_MIN_US  1000u
struct live_flip_entry {
 uint64_t y, uv, flip_us, flip_seq;
 uint32_t width, height, y_stride, uv_stride;
 uint32_t lease_epoch, flags;
};
struct live_flip_obs {
 struct live_flip_entry ring[LIVE_FLIP_RING_CAP]; /* +0   .. +335 (336 B) */
 uint64_t obs_seq;            /* +336 last observed c->irq_seq */
 uint64_t obs_pending_since;  /* +344 IRQ timestamp awaiting s[3+ch] flip */
 uint64_t last_poll_us;       /* +352 timestamp of last C0 snapshot poll (1000 us throttle) */
 uint64_t last_s4;            /* +360 last observed normalized active Y addr (DMA writing) */
 uint64_t last_s4_uv;         /* +368 last observed normalized active UV addr */
 uint64_t last_obs_us;        /* +376 timestamp of last observed actual flip */
 uint64_t flip_seq_head;      /* +384 monotonic completed-flip sequence counter */
 uint64_t last_kept_flip_seq; /* +392 flip_seq of last frame submitted/encoded */
 uint64_t last_irq_stamp;     /* +400 last observed c->irq_timestamp */
 uint32_t ring_head;          /* +408 next write slot (mod LIVE_FLIP_RING_CAP) */
 uint32_t ring_count;         /* +412 valid entries in ring (0..6) */
 uint32_t obs_pending_flip;   /* +416 1 while re-polling for v18 s[3+ch] flip */
 uint32_t flip_obs_count;     /* +420 total completed flips observed */
 uint32_t interp_pushes;      /* +424 intermediate midpoint flips recovered across preemption */
 uint32_t direct_nv12_ok;     /* +428 frames encoded via direct NEON NV12 path */
 uint32_t direct_nv12_retrig; /* +432 0-copy/0-cvac CFR repeat re-triggers */
 uint32_t async_harvests;     /* +436 completed 1-deep async Wave5 AU harvests */
 uint32_t last_w, last_h;     /* +440..+447 geometry of last_s4 */
 uint32_t last_ys, last_uvs;  /* +448..+455 strides of last_s4 */
 uint32_t last_epoch, flip_dt_us; /* +456..+463 cancel_epoch of last_s4 + smoothed flip interval (us) */
 uint64_t fs_last_encoded_gen; /* +464 V71: last FilmSim CPU2 graded frame_gen encoded (0-copy retrig when equal) */
 uint32_t fs_last_mode;        /* +472 V71: last observed filmsim_runtime_live_mode() (0/1/2) */
 uint32_t fs_pad;              /* +476 V71.3: Mode-2 raw-submitted cfr_index+1 tag (0=none) */
};
_Static_assert(sizeof(struct live_flip_entry) == 56u, "live_flip_entry ABI size");
_Static_assert(sizeof(struct live_flip_obs) == 480u && sizeof(struct live_flip_obs) <= 512u,
               "live_flip_obs fits 512 B lv_regs[256..767] partition");
struct live_stream {
 struct au_ring ring;
 live_encoder encoder;
 live_input_au frozen[LIVE_MAX_FRAMES];
 live_transaction_workspace *work;
 uint8_t *i420,*frozen_bytes,*preview,*preview_scratch;
 uint64_t preview_pts[LIVE_PREVIEW_SLOTS],preview_last;uint32_t preview_head,preview_count;
 uint32_t width,height,allocation_requested,bitrate,idr_interval_us;
 uint32_t job,frozen_count,frozen_size,last_duration_us;
 uint64_t last_idr_us,last_pts,clip_start,clip_press,clip_end;
 uint64_t encoded,encode_total_us,encode_max_us,encode_last_us,first_pts;
 uint64_t encode_avg_us,effective_fps_milli,ring_fill,ring_peak;
 uint64_t backlog_dropped,copy_dropped,ignored_presses,low_fps;
 uint64_t snapshot_encoded,mux_start_encoded,mux_end_encoded;
 uint64_t snapshot_first,snapshot_last,geometry_changes;
 uint32_t error,cleanup_failed,mux_started,mux_error;
 void *mux_stack_raw;uint8_t mux_scope[64];
 struct live_diag diag;
 struct live_tune tune;
 uint32_t log_head,log_pad;
 struct live_ev log[LIVE_LOG];
 uintptr_t cache_base;size_t cache_size;
 uint64_t cached_lv_ok,cached_lv_fail,cached_work_ok,cached_work_fail;
 struct {uint64_t gen,s1,s2,s3,s4,s11,s20,s27;} snap[16];uint32_t snap_head,trans_n;
 uint64_t last_y,last_gen;struct {uint64_t from,to;} trans[8];uint64_t picked_prev,picked_fallback;
 uint32_t fr_magic,fr_rows;uint32_t fr_head,fr_alloc_fail;struct live_fr *fr;void *fr_desc;uint64_t fr_torn,fr_checked;
 uint32_t fr_drop,fr_dropped,dcim_copy,dcim_result;uint64_t dcim_copies;
 uint64_t last_kept_y,dup_skipped;
 uint32_t preview_orient,preview_pad; /* v13 */
 /* v52: capture is resampled before encoding. Every scheduled picture is
  * encoded anew; the bitstream ring never duplicates an encoded P picture. */
 uint8_t *cfr_spare;
 uint64_t cfr_epoch,cfr_index,cfr_source_us,cfr_repeats;
 uint32_t cfr_have,cfr_pad; /* cfr_pad: source-reset generation, not padding */
};
static inline void live_log(struct live_stream *s,uint64_t t,uint32_t type,uint32_t a,uint64_t b){
 uint32_t i=__atomic_fetch_add(&s->log_head,1,__ATOMIC_RELAXED)%LIVE_LOG;
 s->log[i].t=t;s->log[i].type=type;s->log[i].a=a;s->log[i].b=b;}
void live_stream_init(struct live_stream *);
void live_stream_step(struct la_capture *,struct live_stream *,uint64_t now,
 int eligible,uint64_t (*clock_us)(void));
void live_stream_release_aux(struct la_capture *,struct live_stream *);
int live_stream_abort_pending_job(struct live_stream *);
int live_stream_convert(uint8_t *,const struct la_source *);
/* Preview frame source: frame i shows the newest preview at or before
 * clip_start + i*LIVE_PREVIEW_STEP_US (the oldest one before the window). */
int live_stream_preview_frame(void *stream,uint32_t index,void *i420,size_t bytes);
/* v46: 0..11 pre-roll twelfths since the last resume, 12 = ready/saving */
uint32_t live_stream_progress(const struct la_capture *c,const struct live_stream *s,uint64_t now);
/* v48: nearest-neighbour NV12 -> I420 into dw x dh (EVF live view) */
int live_stream_convert_scaled(uint8_t *dst,unsigned dw,unsigned dh,const struct la_source *s);
/* V71r2: Fast 5:4 NEON bilinear downscaler (1280x852 NV12 -> 1024x682 NV12) for Mode 2 EVF. */
void live_downscale_1280_to_1024_nv12_neon(
 const uint8_t *src_y,const uint8_t *src_uv,uint32_t src_stride,
 uint8_t *dst_y,uint8_t *dst_uv,uint32_t dst_stride);
/* V70 flip-observed 6-buffer ring helpers (worker task context ONLY, never ISR). */
int live_flip_obs_should_poll(const struct live_flip_obs *obs,uint64_t irq_seq,uint64_t now);
void live_flip_obs_reset(struct live_flip_obs *obs);
void live_flip_obs_ingest_snapshot(struct la_capture *c,struct live_stream *s,
 struct live_flip_obs *obs,uint64_t seq,uint64_t stamp,uint64_t now,uint32_t epoch,
 uintptr_t cur_y,uintptr_t cur_uv,uint32_t width,uint32_t height,
 uint32_t y_stride,uint32_t uv_stride);
#ifndef LIVE_HOST_TEST
struct live_flip_obs *la_stream_flip_obs(struct live_stream *s);
void la_native_observe_flip(struct la_capture *c,struct live_stream *s,uint64_t now,uint64_t (*clock_us)(void));
#else
typedef void (*live_host_observe_fn)(struct la_capture *c,struct live_stream *s,
 struct live_flip_obs *obs,uint64_t now);
void live_host_bind_flip_obs(struct live_flip_obs *obs);
struct live_flip_obs *live_host_get_flip_obs(void);
void live_host_set_observer(live_host_observe_fn fn);
#endif
#endif
