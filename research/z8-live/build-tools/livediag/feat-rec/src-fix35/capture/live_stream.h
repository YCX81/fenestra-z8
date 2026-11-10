#ifndef LIVE_STREAM_H
#define LIVE_STREAM_H
#include "live_capture.h"
#include "../codec/au_ring.h"
#include "../codec/live_transaction.h"
#ifndef LIVE_BITRATE
#define LIVE_BITRATE 12000000u
#endif
#ifndef LIVE_IDR_US
#define LIVE_IDR_US 500000u
#endif
#define LIVE_BITSTREAM_BYTES (5u*1024u*1024u)
/* Small I420 preview kept alongside the bitstream for in-camera playback
 * (PREV.LVP, see codec/preview_writer.h): 15 fps, 64 slots = 4.3 s, which
 * covers the 3 s clip window. Frozen with the clip: no pushes during mux. */
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
 uint32_t picker;           /* live-view buffer choice, see native_capture.c */
 uint32_t icon_refresh_ms;  /* re-present the Live indicator this often, 0 = only on change */
};
struct live_ev {uint64_t t;uint32_t type,a;uint64_t b;};
/* v9 flight record, 64 bytes; result codes in flight_patch.py / livectl.py */
struct live_fr {uint64_t t,seq,after,y,s4,s20;uint32_t conv_us,enc_us;uint16_t torn_y,torn_uv;uint8_t result,picker,nbufs,flags;};
#define LIVE_FR 4096u
#define LIVE_LOG 128u
enum {EV_PRESS=1,EV_ACCEPT,EV_SHORT,EV_IGNORED,EV_SOFT_CANCEL,EV_RELEASE,EV_C0,EV_POOL,EV_GEOMETRY,EV_ALLOC,
 EV_FREEZE,EV_FREEZE_FAIL,EV_MUX_BEGIN,EV_MUX_END,EV_WRITER,EV_READY,EV_ENC_ERR,EV_MUX_ABORT,EV_NOTREADY,EV_ICON,EV_MUX_STAGE,EV_DCIM,EV_UI,EV_FLAG,EV_MODE,EV_W5,EV_LOGSAVE,EV_VIEW,EV_PB};
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
};
static inline void live_log(struct live_stream *s,uint64_t t,uint32_t type,uint32_t a,uint64_t b){
 uint32_t i=__atomic_fetch_add(&s->log_head,1,__ATOMIC_RELAXED)%LIVE_LOG;
 s->log[i].t=t;s->log[i].type=type;s->log[i].a=a;s->log[i].b=b;}
void live_stream_init(struct live_stream *);
void live_stream_step(struct la_capture *,struct live_stream *,uint64_t now,
 int eligible,uint64_t (*clock_us)(void));
int live_stream_convert(uint8_t *,const struct la_source *);
/* PREV.LVP frame source: frame i shows the newest preview at or before
 * clip_start + i*LIVE_PREVIEW_STEP_US (the oldest one before the window). */
int live_stream_preview_frame(void *stream,uint32_t index,void *i420,size_t bytes);
#endif
