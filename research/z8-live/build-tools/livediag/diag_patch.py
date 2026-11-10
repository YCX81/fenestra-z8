#!/usr/bin/env python3
"""Live diagnostic build: per-stage counters and timers in live_stream_step and the worker loop.
Behaviour is unchanged; only counters/timestamps are recorded (struct live_diag at the end of
struct live_stream, magic 'LDG1'). Usage: diag_patch.py SRC_DIR (a copy of src-w5)."""
import sys
from pathlib import Path
src = Path(sys.argv[1])

def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:60], t.count(old))
    p.write_text(t.replace(old, new))

DIAG = '''struct live_diag {
 uint32_t magic,version;
 uint64_t steps,ineligible,new_seq,stale,interval,lease_fail,geometry,valid1_fail,convert_fail,
  valid2_fail,seq_changed,cancelled,busy,ok,enc_error;
 uint64_t last_age_us,max_age_us,last_lease_us,max_lease_us,last_convert_us,max_convert_us,sum_convert_us,n_convert;
 uint64_t last_frames_during_copy,max_frames_during_copy;
 uint64_t y,uv;uint32_t width,height,y_stride,uv_stride;
 uint64_t loops,last_loop_us,max_loop_us,last_ui_us,max_ui_us,last_step_us,max_step_us,last_audio_us,max_audio_us,
  last_sleep_us,max_sleep_us,loop_t0;
 uint64_t snap_y_after,snap_uv_after;
};
'''
sub('capture/live_stream.h', 'struct live_stream {', DIAG + 'struct live_stream {')
sub('capture/live_stream.h', ' void *mux_stack_raw;uint8_t mux_scope[64];\n};',
    ' void *mux_stack_raw;uint8_t mux_scope[64];\n struct live_diag diag;\n};')

M = lambda a, b: f'{a}={b};if({a}>{a.replace("last_", "max_")}){a.replace("last_", "max_")}={a};'
old = '''  if(now-stamp>LA_MAX_GAP_US){c->dropped++;s->copy_dropped++;goto finish;}
  if(stamp<=s->last_pts || (s->last_pts && stamp-s->last_pts<LA_SAMPLE_INTERVAL_US)){c->dropped++;goto finish;}
  struct la_source source={0};
  if(!c->ops.lease_begin(c->ops.context,seq/2,stamp,&source)){c->dropped++;s->copy_dropped++;goto finish;}
  if(!c->descriptor && !allocate(c,s,source.width,source.height)){
   c->ops.lease_end(c->ops.context,&source);c->reason=LA_ALLOC;STORE(&c->phase,LA_FAILED);return;}
  if(source.width!=s->width||source.height!=s->height){s->geometry_changes++;la_cancel(c);c->ops.lease_end(c->ops.context,&source);return;}
  int valid=c->ops.lease_valid(c->ops.context,&source) && live_stream_convert(s->i420,&source);
  valid=valid && c->ops.lease_valid(c->ops.context,&source) && LOAD(&c->irq_seq)==seq && !la_cancelled(c);
  c->ops.lease_end(c->ops.context,&source);
  if(!valid){c->dropped++;s->copy_dropped++;goto finish;}'''
new = '''  struct live_diag *D=&s->diag;D->new_seq++;
  D->last_age_us=now-stamp;if(D->last_age_us>D->max_age_us)D->max_age_us=D->last_age_us;
  if(now-stamp>LA_MAX_GAP_US){D->stale++;c->dropped++;s->copy_dropped++;goto finish;}
  if(stamp<=s->last_pts || (s->last_pts && stamp-s->last_pts<LA_SAMPLE_INTERVAL_US)){D->interval++;c->dropped++;goto finish;}
  struct la_source source={0};
  uint64_t t0=clock_us();
  int began=c->ops.lease_begin(c->ops.context,seq/2,stamp,&source);
  uint64_t t1=clock_us();D->last_lease_us=t1-t0;if(D->last_lease_us>D->max_lease_us)D->max_lease_us=D->last_lease_us;
  if(!began){D->lease_fail++;c->dropped++;s->copy_dropped++;goto finish;}
  D->y=(uint64_t)(uintptr_t)source.y;D->uv=(uint64_t)(uintptr_t)source.uv;D->width=source.width;D->height=source.height;
  D->y_stride=source.y_stride;D->uv_stride=source.uv_stride;
  if(!c->descriptor && !allocate(c,s,source.width,source.height)){
   c->ops.lease_end(c->ops.context,&source);c->reason=LA_ALLOC;STORE(&c->phase,LA_FAILED);return;}
  if(source.width!=s->width||source.height!=s->height){D->geometry++;s->geometry_changes++;la_cancel(c);c->ops.lease_end(c->ops.context,&source);return;}
  int valid=c->ops.lease_valid(c->ops.context,&source);
  if(!valid)D->valid1_fail++;
  else{
   uint64_t t2=clock_us();valid=live_stream_convert(s->i420,&source);uint64_t t3=clock_us();
   D->last_convert_us=t3-t2;if(D->last_convert_us>D->max_convert_us)D->max_convert_us=D->last_convert_us;
   D->sum_convert_us+=t3-t2;D->n_convert++;
   uint64_t after=LOAD(&c->irq_seq);D->last_frames_during_copy=after>seq?(after-seq)/2:0;
   if(D->last_frames_during_copy>D->max_frames_during_copy)D->max_frames_during_copy=D->last_frames_during_copy;
   if(!valid)D->convert_fail++;
   else if(!c->ops.lease_valid(c->ops.context,&source)){valid=0;D->valid2_fail++;}
   else if(LOAD(&c->irq_seq)!=seq){valid=0;D->seq_changed++;}
   else if(la_cancelled(c)){valid=0;D->cancelled++;}
  }
  c->ops.lease_end(c->ops.context,&source);
  if(!valid){c->dropped++;s->copy_dropped++;goto finish;}
  D->ok++;'''
sub('capture/live_stream.c', old, new)
sub('capture/live_stream.c', '  if(rc==LIVE_ENCODER_BUSY){c->dropped++;s->copy_dropped++;goto finish;}\n if(rc){s->error=-rc;',
    '  if(rc==LIVE_ENCODER_BUSY){s->diag.busy++;c->dropped++;s->copy_dropped++;goto finish;}\n if(rc){s->diag.enc_error++;s->error=-rc;')
sub('capture/live_stream.c', ' uint32_t epoch=LOAD(&c->cancel_epoch),job=LOAD(&s->job);\n',
    ' uint32_t epoch=LOAD(&c->cancel_epoch),job=LOAD(&s->job);\n s->diag.magic=0x3147444cu;s->diag.version=1;s->diag.steps++;if(!eligible)s->diag.ineligible++;\n')

old_w = '''  live_ui_poll();r->poll_count++;'''
new_w = '''  struct live_diag *D=&r->stream.diag;uint64_t w0=now_us();
  if(D->loop_t0){D->last_loop_us=w0-D->loop_t0;if(D->last_loop_us>D->max_loop_us)D->max_loop_us=D->last_loop_us;}
  D->loop_t0=w0;D->loops++;
  live_ui_poll();r->poll_count++;
  uint64_t w1=now_us();D->last_ui_us=w1-w0;if(D->last_ui_us>D->max_ui_us)D->max_ui_us=D->last_ui_us;'''
sub('runtime.c', old_w, new_w)
sub('runtime.c', '''  live_stream_step(&r->capture,&r->stream,now_us(),photo && !LOAD(&r->shutdown),now_us);
  uint64_t t=now_us(),lv=LOAD(&r->capture.irq_timestamp);''',
'''  uint64_t w2=now_us();
  live_stream_step(&r->capture,&r->stream,now_us(),photo && !LOAD(&r->shutdown),now_us);
  uint64_t w3=now_us();D->last_step_us=w3-w2;if(D->last_step_us>D->max_step_us)D->max_step_us=D->last_step_us;
  uint64_t t=now_us(),lv=LOAD(&r->capture.irq_timestamp);''')
sub('runtime.c', '''  live_audio_step(r,photo && !LOAD(&r->shutdown) && LOAD(&r->requested) && (lv_on || LOAD(&r->stream.job)),t);
  FN(0x40187b00,int32_t (*)(uint32_t))(5);''',
'''  live_audio_step(r,photo && !LOAD(&r->shutdown) && LOAD(&r->requested) && (lv_on || LOAD(&r->stream.job)),t);
  uint64_t w4=now_us();D->last_audio_us=w4-t;if(D->last_audio_us>D->max_audio_us)D->max_audio_us=D->last_audio_us;
  FN(0x40187b00,int32_t (*)(uint32_t))(5);
  uint64_t w5=now_us();D->last_sleep_us=w5-w4;if(D->last_sleep_us>D->max_sleep_us)D->max_sleep_us=D->last_sleep_us;''')
print('diag patch applied to', src)
