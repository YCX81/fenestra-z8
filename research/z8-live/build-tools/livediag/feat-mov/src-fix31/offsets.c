#include <stdio.h>
#include <stddef.h>
#include "runtime.h"
#define FIELD(f) printf("\"" #f "\":%zu,",offsetof(struct live_runtime,f))
int main(void){printf("{");
FIELD(magic);FIELD(size);FIELD(requested);FIELD(task_started);FIELD(task_error);FIELD(poll_count);FIELD(full_presses);FIELD(writer_returns);FIELD(jpeg_returns);FIELD(last_native_path);FIELD(transaction_count);FIELD(transaction_error);FIELD(encode_started_us);FIELD(encode_finished_us);FIELD(frame_encode_started_us);FIELD(frame_encode_finished_us);FIELD(frame_encode_target);FIELD(frame_encode_completed);FIELD(debug_ticket_path);FIELD(debug_dir_path);FIELD(debug_path_result);FIELD(debug_fs_error);FIELD(debug_dir_stage);
FIELD(capture.phase);FIELD(capture.reason);FIELD(capture.irq_seq);FIELD(capture.copied);FIELD(capture.dropped);FIELD(capture.actual_bytes);FIELD(capture.count);FIELD(capture.accepted_presses);FIELD(capture.rejected_presses);FIELD(capture.press_us);
FIELD(stream.width);FIELD(stream.height);FIELD(stream.allocation_requested);FIELD(stream.bitrate);FIELD(stream.idr_interval_us);FIELD(stream.job);FIELD(stream.frozen_count);FIELD(stream.frozen_size);FIELD(stream.last_duration_us);FIELD(stream.error);FIELD(stream.cleanup_failed);FIELD(stream.mux_started);FIELD(stream.mux_error);
FIELD(stream.first_pts);FIELD(stream.last_pts);FIELD(stream.encoded);FIELD(stream.encode_total_us);FIELD(stream.encode_max_us);FIELD(stream.encode_last_us);FIELD(stream.encode_avg_us);FIELD(stream.effective_fps_milli);FIELD(stream.ring_fill);FIELD(stream.ring_peak);FIELD(stream.backlog_dropped);FIELD(stream.copy_dropped);FIELD(stream.ignored_presses);FIELD(stream.low_fps);FIELD(stream.snapshot_encoded);FIELD(stream.mux_start_encoded);FIELD(stream.mux_end_encoded);FIELD(stream.snapshot_first);FIELD(stream.snapshot_last);FIELD(stream.clip_press);FIELD(stream.clip_end);FIELD(stream.geometry_changes);
printf("\"struct_bytes\":%zu}\n",sizeof(struct live_runtime));return 0;}
