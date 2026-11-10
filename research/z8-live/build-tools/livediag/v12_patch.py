#!/usr/bin/env python3
"""v12 Live fix on top of v11 (hardware 11-07, v10 handheld clips judder + residual tearing).
Root cause: picker 3 ("address predecessor among learned buffers") learned its buffer set from the
*picked* address (diag.bufs <- source.y). With picker 3 as the default from boot, the first pick
was the only buffer ever learned, so every frame copied that same buffer (flight log: 1296/1296 kept
frames y=0xa1fb3f800, nbufs=1). The hardware rewrites it every 6th frame, so the clip showed a new
picture only ~5 times per second (judder) and a copy sometimes overlapped the rewrite (tearing).
(The v9 picker-3 test only looked right because picker 0 had already taught all six buffers.)
  1. Learn the rotation set from the raw snapshot word s[4] in snapshot_pick_c (record path).
  2. Never encode the same buffer twice in a row: if the picked buffer equals the last kept one,
     drop the frame as 'duplicate' (result 15) instead of repeating an old picture.
Usage: v12_patch.py SRC_DIR (a copy of src-fix11)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])


def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))


N = 'capture/native_capture.c'
sub(N, '     st->last_y=cur;st->last_gen=generation;}',
    '     st->last_y=cur;st->last_gen=generation;\n'
    '     /* v12: the buffer set comes from the snapshot word itself, never from the picked address. */\n'
    '     {unsigned k;for(k=0;k<st->diag.nbufs&&st->diag.bufs[k]!=cur;k++){}\n'
    '      if(k==st->diag.nbufs&&st->diag.nbufs<8&&cur)st->diag.bufs[st->diag.nbufs++]=cur;}}')
H = 'capture/live_stream.h'
sub(H, ' uint32_t fr_drop,fr_dropped,dcim_copy,dcim_result;uint64_t dcim_copies;\n};',
    ' uint32_t fr_drop,fr_dropped,dcim_copy,dcim_result;uint64_t dcim_copies;\n'
    ' uint64_t last_kept_y,dup_skipped;\n};')
C = 'capture/live_stream.c'
sub(C, "  fr.y=D->y;{uint32_t sh=s->snap_head;if(sh){fr.s4=s->snap[(sh-1)%16u].s4;fr.s20=s->snap[(sh-1)%16u].s20;}}",
    "  fr.y=D->y;{uint32_t sh=s->snap_head;if(sh){fr.s4=s->snap[(sh-1)%16u].s4;fr.s20=s->snap[(sh-1)%16u].s20;}}\n"
    "  if(s->last_kept_y&&D->y==s->last_kept_y){c->ops.lease_end(c->ops.context,&source);s->dup_skipped++;fr.flags|=4;c->dropped++;goto finish;}")
sub(C, ' if(f->flags&1)r=14;else if(D->busy>d0->busy)r=13;',
    ' if(f->flags&4)r=15;else if(f->flags&1)r=14;else if(D->busy>d0->busy)r=13;')
sub(C, '  s->encoded++;s->encode_total_us+=elapsed;',
    '  s->last_kept_y=D->y;s->encoded++;s->encode_total_us+=elapsed;')
print('v12 Live patch applied to', src)
