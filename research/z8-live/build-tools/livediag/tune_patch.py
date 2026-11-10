#!/usr/bin/env python3
"""Live tune + event log on top of diag/fix1/fix2/fix3.
struct live_tune (magic 'LTN1', in struct live_stream after diag): values the worker reads every
step, writable over USB debug without reflashing. struct live_ev log[128] (head counter) records
press / cancel / geometry / allocation / freeze / mux events with now_us() stamps.
Usage: tune_patch.py SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))

H = '''struct live_tune {
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
 uint32_t reserved[5];
};
struct live_ev {uint64_t t;uint32_t type,a;uint64_t b;};
#define LIVE_LOG 128u
enum {EV_PRESS=1,EV_ACCEPT,EV_SHORT,EV_IGNORED,EV_SOFT_CANCEL,EV_RELEASE,EV_C0,EV_POOL,EV_GEOMETRY,EV_ALLOC,
 EV_FREEZE,EV_FREEZE_FAIL,EV_MUX_BEGIN,EV_MUX_END,EV_WRITER,EV_READY,EV_ENC_ERR,EV_MUX_ABORT,EV_NOTREADY};
'''
sub('capture/live_stream.h', 'struct live_stream {', H + 'struct live_stream {')
sub('capture/live_stream.h', ' struct live_diag diag;\n};',
    ' struct live_diag diag;\n struct live_tune tune;\n uint32_t log_head,log_pad;\n struct live_ev log[LIVE_LOG];\n};\n'
    'static inline void live_log(struct live_stream *s,uint64_t t,uint32_t type,uint32_t a,uint64_t b){\n'
    ' uint32_t i=__atomic_fetch_add(&s->log_head,1,__ATOMIC_RELAXED)%LIVE_LOG;\n'
    ' s->log[i].t=t;s->log[i].type=type;s->log[i].a=a;s->log[i].b=b;}')
sub('capture/live_stream.c', ' memset(s,0,sizeof(*s));s->bitrate=LIVE_BITRATE;s->idr_interval_us=LIVE_IDR_US;',
    ''' memset(s,0,sizeof(*s));s->bitrate=LIVE_BITRATE;s->idr_interval_us=LIVE_IDR_US;
 s->tune=(struct live_tune){0x314e544cu,1,-1,1,LA_WINDOW_US,LA_SAMPLE_INTERVAL_US,LA_MAX_GAP_US,1,3000000u,1,250000u,{0,0,0,0,0}};''')
C = 'capture/live_stream.c'
sub(C, '  int found=!job && press>=LA_WINDOW_US && !au_ring_find_start(&s->ring,press-LA_WINDOW_US,&start);',
       '  uint32_t W=s->tune.window_us;\n  int found=!job && press>=W && !au_ring_find_start(&s->ring,press-W,&start);\n  if(found)live_log(s,now,EV_ACCEPT,start,press);')
sub(C, '  if(!job && !found){', '  if(!job && !found && s->tune.short_start){')
sub(C, 'if(e->idr){start=i;found=1;s->diag.short_start++;break;}}',
       'if(e->idr){start=i;found=1;s->diag.short_start++;live_log(s,now,EV_SHORT,i,press);break;}}')
sub(C, '   s->clip_press=press;s->clip_end=press+LA_WINDOW_US;', '   s->clip_press=press;s->clip_end=press+W;')
sub(C, '  }else{s->ignored_presses++;c->rejected_presses++;}',
       '  }else{s->ignored_presses++;c->rejected_presses++;live_log(s,now,EV_IGNORED,s->ring.count|job<<16,press);}')
sub(C, '  if(now-stamp>LA_MAX_GAP_US){D->stale++;', '  if(now-stamp>s->tune.stale_us){D->stale++;')
sub(C, '(s->last_pts && stamp-s->last_pts<LA_SAMPLE_INTERVAL_US)){D->interval++;', '(s->last_pts && stamp-s->last_pts<s->tune.sample_us)){D->interval++;')
sub(C, '''    int late=!same && !(now_seq&1) && D->nbufs>=3 && passed<=D->nbufs-2 &&''',
       '''    int64_t lim=s->tune.late_max<0?(D->nbufs>=3?(int64_t)D->nbufs-2:0):(s->tune.late_max>8?8:s->tune.late_max);
    int late=!same && !(now_seq&1) && lim>0 && passed<=(uint64_t)lim &&''')
sub(C, ''' if(eligible && LOAD(&c->requested) && epoch!=c->worker_epoch) {''',
       ''' if(eligible && LOAD(&c->requested) && epoch!=c->worker_epoch && s->tune.soft_cancel) {''')
sub(C, '  c->worker_epoch=epoch;s->diag.soft_cancel++;', '  c->worker_epoch=epoch;s->diag.soft_cancel++;live_log(s,now,EV_SOFT_CANCEL,epoch,job);')
sub(C, ''' if(!eligible || !LOAD(&c->requested)) {''', ''' if(!eligible || !LOAD(&c->requested) || epoch!=c->worker_epoch) {''')
sub(C, '  release(c,s);job=0;c->worker_epoch=epoch;', '  if(c->descriptor||job)live_log(s,now,EV_RELEASE,eligible|LOAD(&c->requested)<<1,job);\n  release(c,s);job=0;c->worker_epoch=epoch;')
sub(C, 'c->reason=LA_ALLOC;D->alloc_retry++;', 'c->reason=LA_ALLOC;D->alloc_retry++;if(D->alloc_retry<64)live_log(s,now,EV_ALLOC,0,0);')
sub(C, '   if(!job){if(!D->geometry_since)D->geometry_since=now?now:1;',
       '   if(!D->geometry_since||job)live_log(s,now,EV_GEOMETRY,source.width<<16|source.height,job);\n   if(!job){if(!D->geometry_since)D->geometry_since=now?now:1;')
sub(C, '    else if(now-D->geometry_since>3000000u){', '    else if(now-D->geometry_since>s->tune.geometry_reset_us){')
sub(C, ' if(rc){s->diag.enc_error++;', ' if(rc){s->diag.enc_error++;live_log(s,now,EV_ENC_ERR,(uint32_t)-rc,0);')
sub(C, ' if(job==1 && now>=s->clip_end)freeze(c,s);', ' if(job==1 && now>=s->clip_end){freeze(c,s);live_log(s,now,LOAD(&s->job)==2?EV_FREEZE:EV_FREEZE_FAIL,s->frozen_count,s->frozen_size|(uint64_t)s->error<<32);}')
sub(C, '''  if(now>=LA_WINDOW_US && s->last_pts && now-s->last_pts<=250000 &&
    !au_ring_find_start(&s->ring,now-LA_WINDOW_US,&start))STORE(&c->phase,LA_READY);''',
'''  if(now>=s->tune.window_us && s->last_pts && now-s->last_pts<=s->tune.ready_fresh_us &&
    !au_ring_find_start(&s->ring,now-s->tune.window_us,&start)){if(c->phase!=LA_READY)live_log(s,now,EV_READY,s->ring.count,0);STORE(&c->phase,LA_READY);}''')
R = 'runtime.c'
sub(R, ''' __atomic_add_fetch(&r->full_presses,1,__ATOMIC_RELAXED);''',
       ''' __atomic_add_fetch(&r->full_presses,1,__ATOMIC_RELAXED);
 live_log(&r->stream,now_us(),EV_PRESS,LOAD(&r->capture.phase),LOAD(&r->stream.job));''')
sub(R, ' struct live_runtime *r=state();if(r && base==0x15431000)la_cancel(&r->capture);',
       ' struct live_runtime *r=state();if(r && base==0x15431000){la_cancel(&r->capture);live_log(&r->stream,now_us(),EV_C0,0,0);}')
sub(R, ' struct live_runtime *r=state();if(r && pool==UINT64_C(0xa03b6b538))la_cancel(&r->capture);',
       ' struct live_runtime *r=state();if(r && pool==UINT64_C(0xa03b6b538)){la_cancel(&r->capture);live_log(&r->stream,now_us(),EV_POOL,0,0);}')
sub(R, ' if(!LOAD(&r->capture.requested)||LOAD(&r->shutdown)){STORE(&r->stream.job,4);return;}',
       ' if(!LOAD(&r->capture.requested)||LOAD(&r->shutdown)){STORE(&r->stream.job,4);live_log(&r->stream,now_us(),EV_MUX_ABORT,0,0);return;}')
sub(R, ' r->encode_started_us=now_us();rc=live_write_pair_aus(',
       ' live_log(&r->stream,now_us(),EV_MUX_BEGIN,count,t.generation);\n r->encode_started_us=now_us();rc=live_write_pair_aus(')
sub(R, ' r->transaction_error=(uint32_t)(-rc);if(!rc)r->transaction_count++;',
       ' r->transaction_error=(uint32_t)(-rc);if(!rc)r->transaction_count++;\n live_log(&r->stream,now_us(),EV_MUX_END,(uint32_t)(-rc),(uint64_t)fc.last_native_error|(uint64_t)fc.dir_stage<<32);')
sub(R, '''  r->writer_result=result;''', '''  r->writer_result=result;live_log(&r->stream,now_us(),EV_WRITER,result,r->writer_path_valid);''')
sub(R, '  live_audio_step(r,photo && !LOAD(&r->shutdown)', '  live_audio_step(r,r->stream.tune.audio && photo && !LOAD(&r->shutdown)')
print('tune patch applied to', src)
