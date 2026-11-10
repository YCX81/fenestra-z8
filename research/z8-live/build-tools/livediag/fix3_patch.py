#!/usr/bin/env python3
"""Live fix v3 on top of diag + fix1 + fix2 (hardware 2026-11-05: every press was lost because the
C0 start/stop/hold and still-pool hooks cancel the capture at the shutter, and the worker then
released the ring, the clip job and the press mailbox).
1. A cancel while Live stays requested and eligible only invalidates the frame in flight; the
   encoded ring, the clip job and the press survive. Mode change / Live off still release.
2. The mux aborts only when Live is switched off (not on a soft cancel).
3. Encoder timing: struct live_w5 gains t_in/t_wait/t_out/t_parse (last, sum) and n_timed at its end.
Diag gains soft_cancel. Usage: fix3_patch.py SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))

sub('capture/live_stream.h', ' uint64_t alloc_retry,short_start,geometry_skip,geometry_since;\n};',
    ' uint64_t alloc_retry,short_start,geometry_skip,geometry_since;\n uint64_t soft_cancel;\n};')
sub('capture/live_stream.c', ''' if(!eligible || !LOAD(&c->requested) || epoch!=c->worker_epoch) {
  if(!eligible && epoch==c->worker_epoch)epoch=__atomic_add_fetch(&c->cancel_epoch,1,__ATOMIC_ACQ_REL);''',
''' if(eligible && LOAD(&c->requested) && epoch!=c->worker_epoch) {
  /* C0 start/stop/hold and the still pool cancel at every shutter: the live-view source is
   * void, but encoded AUs are our own memory. Drop only the frame in flight. */
  c->worker_epoch=epoch;s->diag.soft_cancel++;
 }
 if(!eligible || !LOAD(&c->requested)) {
  if(!eligible && epoch==c->worker_epoch)epoch=__atomic_add_fetch(&c->cancel_epoch,1,__ATOMIC_ACQ_REL);''')
sub('capture/live_stream.c', 's->diag.version=3;', 's->diag.version=4;')
sub('runtime.c', 'static int live_encode_cancel(void *p){return la_cancelled(&((struct live_runtime *)p)->capture);}',
    'static int live_encode_cancel(void *p){struct live_runtime *r=p;return !LOAD(&r->capture.requested)||LOAD(&r->shutdown);}')
sub('runtime.c', ' if(la_cancelled(&r->capture)){STORE(&r->stream.job,4);return;}',
    ' if(!LOAD(&r->capture.requested)||LOAD(&r->shutdown)){STORE(&r->stream.job,4);return;}')

# encoder timing
sub('codec/live_w5enc.c', '''    uint8_t wrap[W5_AU_CAP] __attribute__((aligned(16)));   /* AU that wrapped the 4 MiB ring */
};''', '''    uint8_t wrap[W5_AU_CAP] __attribute__((aligned(16)));   /* AU that wrapped the 4 MiB ring */
    uint64_t t_in, t_wait, t_out, t_parse, s_in, s_wait, s_out, s_parse, n_timed;   /* diag, us */
};''')
sub('codec/live_w5enc.c', '''    unsigned force = !w->frames || pts - *last_idr >= interval;''',
'''    uint64_t t_in_end = now_us(); w->t_in = t_in_end - t_in0; w->s_in += w->t_in;
    unsigned force = !w->frames || pts - *last_idr >= interval;''')
sub('codec/live_w5enc.c', '''    uint8_t *y = (uint8_t *)(uintptr_t)w->slot[slot], *c = y + (size_t)width * ((height + 31) & ~31u);''',
'''    uint64_t t_in0 = now_us();
    uint8_t *y = (uint8_t *)(uintptr_t)w->slot[slot], *c = y + (size_t)width * ((height + 31) & ~31u);''')
sub('codec/live_w5enc.c', '''            uint64_t bs0 = w->region[R_BS0], end = w->region[R_BS1] + region_size[R_BS1];''',
'''            uint64_t t_done = now_us(); w->t_wait = t_done - start; w->s_wait += w->t_wait;
            uint64_t bs0 = w->region[R_BS0], end = w->region[R_BS1] + region_size[R_BS1];''')
sub('codec/live_w5enc.c', '''            size_t n;
            if (live_h264_idr_au(&w->idr, src, size, w->au, W5_AU_CAP, &n, idr)) return -23;''',
'''            size_t n;
            uint64_t t_parse0 = now_us(); w->t_out = t_parse0 - t_done; w->s_out += w->t_out;
            if (live_h264_idr_au(&w->idr, src, size, w->au, W5_AU_CAP, &n, idr)) return -23;
            w->t_parse = now_us() - t_parse0; w->s_parse += w->t_parse; w->n_timed++;''')
print('fix3 patch applied to', src)
