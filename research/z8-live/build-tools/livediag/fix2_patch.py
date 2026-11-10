#!/usr/bin/env python3
"""Live fix v2 on top of diag_patch.py + fix_patch.py (hardware findings 2026-11-05):
1. Wave5 input/output through 64-byte ldp/stp bursts (VPU slot and bitstream buffers are uncached).
2. A frame whose geometry differs from the stream is skipped; the ring and allocation are kept.
3. A failed allocation leaves Live waiting (retried on the next frame) instead of LA_FAILED forever.
4. When no IDR is 1.5 s before the press, the clip starts at the earliest IDR before the press.
New diag counters: alloc_retry, short_start. Usage: fix2_patch.py SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))

BURST = r'''/* 64-byte bursts with ldp/stp (16 B per access) for uncached buffers; bytes when the two
 * pointers do not share 16-byte alignment, byte prefix until they are aligned. */
static void w5_burst(uint8_t *d,const uint8_t *s,size_t n) {
 if((((uintptr_t)d^(uintptr_t)s)&15)==0){
  while(n && ((uintptr_t)s&15)){*d++=*s++;n--;}
  while(n>=64){
   __asm__ volatile("ldp x9,x10,[%1]\n ldp x11,x12,[%1,#16]\n ldp x13,x14,[%1,#32]\n ldp x15,x16,[%1,#48]\n"
                    "stp x9,x10,[%0]\n stp x11,x12,[%0,#16]\n stp x13,x14,[%0,#32]\n stp x15,x16,[%0,#48]\n"
                    ::"r"(d),"r"(s):"x9","x10","x11","x12","x13","x14","x15","x16","memory");
   d+=64;s+=64;n-=64;}
 }
 while(n--)*d++=*s++;
}
'''
sub('codec/live_w5enc.c', 'static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }',
    BURST + 'static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }')
sub('codec/live_w5enc.c', '''    memcpy(y, in, (size_t)width * height);
    for (unsigned r = 0; r < height / 2; r++)
        for (unsigned x = 0; x < width / 2; x++) {
            c[(size_t)r * width + 2 * x] = u[(size_t)r * (width / 2) + x];
            c[(size_t)r * width + 2 * x + 1] = v[(size_t)r * (width / 2) + x];
        }''', '''    w5_burst(y, in, (size_t)width * height);
    {
        uint8_t row[4096] __attribute__((aligned(16)));
        for (unsigned r = 0; r < height / 2; r++) {
            for (unsigned x = 0; x < width / 2; x++) {
                row[2 * x] = u[(size_t)r * (width / 2) + x];
                row[2 * x + 1] = v[(size_t)r * (width / 2) + x];
            }
            w5_burst(c + (size_t)r * width, row, width);
        }
    }''')
sub('codec/live_w5enc.c', '''                memcpy(w->wrap, (const uint8_t *)(uintptr_t)a1, n1);
                memcpy(w->wrap + n1, (const uint8_t *)(uintptr_t)a2, n2);
                src = w->wrap;
            } else if (addr + size > end) return -22;''', '''                w5_burst(w->wrap, (const uint8_t *)(uintptr_t)a1, n1);
                w5_burst(w->wrap + n1, (const uint8_t *)(uintptr_t)a2, n2);
                src = w->wrap;
            } else if (addr + size > end) return -22;
            else if (size + 16 <= W5_AU_CAP) {
                /* same 16-byte phase as the source so the copy runs in bursts */
                uint8_t *dst = w->wrap + (addr & 15);
                w5_burst(dst, src, size);
                src = dst;
            }''')

# stream: geometry, allocation, short start
sub('capture/live_stream.h', ' uint64_t bufs[8];uint32_t nbufs,version2;uint64_t accepted_late,fast_rows,slow_rows;\n};',
    ' uint64_t bufs[8];uint32_t nbufs,version2;uint64_t accepted_late,fast_rows,slow_rows;\n uint64_t alloc_retry,short_start,geometry_skip,geometry_since;\n};')
sub('capture/live_stream.c', '''  if(!c->descriptor && !allocate(c,s,source.width,source.height)){
   c->ops.lease_end(c->ops.context,&source);c->reason=LA_ALLOC;STORE(&c->phase,LA_FAILED);return;}
  if(source.width!=s->width||source.height!=s->height){D->geometry++;s->geometry_changes++;la_cancel(c);c->ops.lease_end(c->ops.context,&source);return;}''',
'''  if(!c->descriptor && !allocate(c,s,source.width,source.height)){
   /* The native pool is gated while a still is processed: wait and retry on a later frame. */
   c->ops.lease_end(c->ops.context,&source);c->reason=LA_ALLOC;D->alloc_retry++;c->dropped++;goto finish;}
  if(source.width!=s->width||source.height!=s->height){
   /* Live view changes geometry around a still capture: skip those frames, keep the clip. */
   D->geometry++;D->geometry_skip++;s->geometry_changes++;c->ops.lease_end(c->ops.context,&source);c->dropped++;
   /* A lasting change (not around a capture, no clip in progress) restarts Live as before. */
   if(!job){if(!D->geometry_since)D->geometry_since=now?now:1;
    else if(now-D->geometry_since>3000000u){D->geometry_since=0;la_cancel(c);return;}}
   goto finish;}
  D->geometry_since=0;''')
sub('capture/live_stream.c', '''  if(!job && press>=LA_WINDOW_US && !au_ring_find_start(&s->ring,press-LA_WINDOW_US,&start)){''',
'''  int found=!job && press>=LA_WINDOW_US && !au_ring_find_start(&s->ring,press-LA_WINDOW_US,&start);
  if(!job && !found){
   /* No IDR 1.5 s back (slow encode or a young ring): start at the earliest IDR before the press. */
   for(unsigned i=0;i<s->ring.count;i++){const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
    if(e->pts>press)break;if(e->idr){start=i;found=1;s->diag.short_start++;break;}}
  }
  if(found){''')
sub('capture/live_stream.c', 's->diag.version=2;', 's->diag.version=3;')
print('fix2 patch applied to', src)
