#!/usr/bin/env python3
"""v8: selectable live-view buffer picker (tune.picker, USB-writable) + snapshot ring.
Hardware 11-05 (v7): MOV frames tear (top band from a newer frame) even with late_max=0, so the
C0 snapshot word Live uses (s[4]/s[11]) can name a buffer the hardware is writing. Modes:
  0 s[4]/s[11] (as before)        1 s[20]/s[27]
  2 predecessor of s[4] in the rotation learned from consecutive generations
  3 predecessor of s[4] by address among the learned buffers (ascending rotation)
The last 16 snapshots (generation, s[1..4], s[11], s[20], s[27]) are kept in stream.snap[].
Usage: picker_patch.py SRC_DIR (after io_patch)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))
sub('capture/live_stream.h', ' uint32_t cached_lv,cached_work,cached_slot,cached_bs; /* cacheable-alias switches, default 1 */\n uint32_t reserved[1];',
    ' uint32_t cached_lv,cached_work,cached_slot,cached_bs; /* cacheable-alias switches, default 1 */\n uint32_t picker;           /* live-view buffer choice, see native_capture.c */')
sub('capture/live_stream.h', ' uint64_t cached_lv_ok,cached_lv_fail,cached_work_ok,cached_work_fail;\n};',
    ' uint64_t cached_lv_ok,cached_lv_fail,cached_work_ok,cached_work_fail;\n'
    ' struct {uint64_t gen,s1,s2,s3,s4,s11,s20,s27;} snap[16];uint32_t snap_head,trans_n;\n'
    ' uint64_t last_y,last_gen;struct {uint64_t from,to;} trans[8];uint64_t picked_prev,picked_fallback;\n};')
N = 'capture/native_capture.c'
sub(N, '#include "native_capture.h"', '#include "native_capture.h"\n#include <stddef.h>\n#include "runtime.h"')
sub(N, 'static int snapshot_pick(uint64_t generation, uintptr_t *y, uintptr_t *uv) {\n    uint64_t s[33]={0};\n    (void)generation;',
'''static uintptr_t norm(uintptr_t a){return a>=UINT64_C(0xc00000000)?a-UINT64_C(0x400000000):a;}
static int snapshot_pick_c(struct la_capture *c,int record,uint64_t generation, uintptr_t *y, uintptr_t *uv) {
    uint64_t s[33]={0};
    struct live_runtime *r=(struct live_runtime *)((uint8_t *)c-offsetof(struct live_runtime,capture));
    struct live_stream *st=&r->stream;uint32_t mode=st->tune.picker;''')
sub(N, '''#if LA_PICKER == 0
    *y=s[4];*uv=s[11]; /* C0+a0/a8 */''', '''    if(record){uint32_t i=st->snap_head++%16u;st->snap[i].gen=generation;st->snap[i].s1=s[1];st->snap[i].s2=s[2];st->snap[i].s3=s[3];
     st->snap[i].s4=s[4];st->snap[i].s11=s[11];st->snap[i].s20=s[20];st->snap[i].s27=s[27];
     uintptr_t cur=norm(s[4]);
     if(st->last_y && generation==st->last_gen+1 && cur!=st->last_y){unsigned k;for(k=0;k<st->trans_n&&st->trans[k].from!=st->last_y;k++){}
      if(k<8){st->trans[k].from=st->last_y;st->trans[k].to=cur;if(k==st->trans_n)st->trans_n++;}}
     st->last_y=cur;st->last_gen=generation;}
#if LA_PICKER == 0
    if(mode==1){*y=s[20];*uv=s[27];}
    else{*y=s[4];*uv=s[11];} /* C0+a0/a8 */
    if(mode==2||mode==3){
     uintptr_t cur=norm(*y),d=norm(*uv)-cur,prev=0;
     if(mode==2){for(unsigned k=0;k<st->trans_n;k++)if(st->trans[k].to==cur){prev=st->trans[k].from;break;}}
     else{uintptr_t best=0,mx=0;for(unsigned k=0;k<st->diag.nbufs;k++){uintptr_t b=st->diag.bufs[k];if(b>mx)mx=b;if(b<cur&&b>best)best=b;}
      prev=best?best:(mx&&mx!=cur?mx:0);}
     if(prev){*y=prev;*uv=prev+d;if(record)st->picked_prev++;}else if(record)st->picked_fallback++;
    }''')
sub(N, '    if(!la_native_geometry(&g)||!snapshot_pick(generation,&y,&uv))return 0;',
       '    if(!la_native_geometry(&g)||!snapshot_pick_c(c,1,generation,&y,&uv))return 0;')
sub(N, '        snapshot_pick(s->generation,&y,&uv) && y==(uintptr_t)s->y && uv==(uintptr_t)s->uv;',
       '        snapshot_pick_c(c,0,s->generation,&y,&uv) && y==(uintptr_t)s->y && uv==(uintptr_t)s->uv;')
sub('capture/live_stream.c', 's->diag.version=7;', 's->diag.version=8;')
sub('capture/live_stream.c', '1,3000000u,1,250000u,1,1,1,1,{0}};', '1,3000000u,1,250000u,1,1,1,1,0};')
print('picker patch applied to', src)
