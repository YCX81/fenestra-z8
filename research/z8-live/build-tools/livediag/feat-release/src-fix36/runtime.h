#ifndef LIVE_RUNTIME_H
#define LIVE_RUNTIME_H
#include <stdint.h>
#include "capture/live_capture.h"
#include "capture/live_stream.h"
#include "audio/live_audio_z8.h"
#define LIVE_RUNTIME_MAGIC 0x4c415231u
struct live_ticket {
 uint64_t descriptor, generation, size, success_us;
 uint32_t fd, file_id, card_epoch, open, success, closed;
 char path[32];
};
struct live_runtime {
 uint32_t magic, size;
 struct la_capture capture;
 uint32_t requested, task_started, task_error, shutdown, card_epoch;
 uint64_t poll_count, full_presses, writer_returns, jpeg_returns;
 uint32_t writer_active, writer_file_id, writer_path_valid, writer_result;
 char writer_path[32], last_native_path[32];
 void *worker_stack_raw;
 uint8_t task_scope[64];
 uint32_t ticket_lock, ticket_generation, press_ticket_floor, ticket_drops;
 uint32_t transaction_count, transaction_error;
 uint64_t encode_started_us, encode_finished_us;
 struct live_ticket tickets[8];
 /* Guest-visible counters; duration includes per-frame MOV writes/yields. */
 uint64_t frame_encode_started_us, frame_encode_finished_us;
 uint32_t frame_encode_target, frame_encode_completed;
 char debug_ticket_path[32];
 char debug_dir_path[128];
 uint32_t debug_path_result, debug_fs_error, debug_dir_stage;
 struct live_stream stream;
 /* Photo-mode audio (appended: earlier offsets unchanged). audio_phase:
  * 0 none, 1 marked/awaiting freeze, 2 frozen for the mux, 3 clip without audio. */
 struct live_audio audio;
 struct live_audio_z8 audio_z8;
 struct live_audio_info audio_info;
 uint32_t audio_phase,audio_error,audio_starts,audio_tracks,audio_skipped;
 uint64_t audio_retry_us;
 /* v13 (appended): boot-flag park/restore and saved event log */
 uint32_t flag_req,flag_n,flag_parks,flag_restores,flag_err,last_photo;
 /* v36: former log_pad is a cached RMW word; boot zeroes it once.
  * Do not reset on encoder close/reopen. Runtime size/other offsets unchanged. */
 uint32_t log_saved_head,log_saves,log_err,w5_close_lock;
 uint64_t flag_t,log_saved_t;
 uint8_t flag_buf[256],log_io[512];
 uint32_t crumb_head,crumb_pad;struct live_ev crumbs[32];
 uint32_t lv_channel,lv_pad; /* v14: live-view scaler output Live copies (0..3), default 1 */
 uint32_t lv_geo[4][6]; /* v14: per output: width, height, y stride, uv stride, nv12, valid */
 uint64_t lv_snap[4],lv_geo_t; /* v14: last snapshot Y address per output, time of lv_geo */
 uint32_t lv_regs[228],lv_regs_n; /* v16: scaler register record, order = v16_patch.LV_REGS */
 uint32_t lv_regs_on,lv_regs_pad; /* v16: 1 = record (set over USB); default 0, nothing is read */
 uint32_t ch2_req,ch2_state,ch2_applies,ch2_restores,ch2_reapplies,ch2_pad,ch2_saved[12]; /* v17 */
 uint64_t last_view,last_pb_b;uint32_t last_view_is,last_pb_a; /* v23 */
 uint32_t vhead,vpad;struct live_ev vring[64]; /* v23: view/playback changes, RAM only (no card saves) */
};
void live_irq_notify(uint32_t event,uint64_t status);
void live_press_notify(void);
uint32_t live_writer_wrap(uint32_t mode,uint32_t format,const void *record);
uint32_t live_open_wrap(void *file);
#endif
