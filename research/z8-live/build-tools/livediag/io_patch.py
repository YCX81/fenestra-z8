#!/usr/bin/env python3
"""v7: native file I/O rejects buffers at the cacheable alias (VA+0x400000000); hardware 11-05:
the mux failed with -1 at its first read of the native JPG into the aliased workspace, and
succeeded to write once cached_work was 0. Hand the native calls the original (uncached) VA of
the same PA; cache maintenance on the alias stays (clean before write, clean+invalidate before
read, invalidate after). Usage: io_patch.py SRC_DIR (after cache_patch)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
F = src / 'codec/native_files.c'; t = F.read_text()
old_r = ' la_dc_civac((uintptr_t)p,n);uint32_t r=READ(fd,p,(uint32_t)n,&got);la_dc_ivac((uintptr_t)p,n);*actual=got;return result(c,r);'
new_r = ' la_dc_civac((uintptr_t)p,n);uint32_t r=READ(fd,la_native_va(p),(uint32_t)n,&got);la_dc_ivac((uintptr_t)p,n);*actual=got;return result(c,r);'
old_w = ' la_dc_cvac((uintptr_t)p,n);uint32_t r=WRITE(fd,p,(uint32_t)n,&got,0);*actual=got;return result(c,r);'
new_w = ' la_dc_cvac((uintptr_t)p,n);uint32_t r=WRITE(fd,la_native_va(p),(uint32_t)n,&got,0);*actual=got;return result(c,r);'
assert t.count(old_r) == 1 and t.count(old_w) == 1
t = t.replace(old_r, new_r).replace(old_w, new_w)
t = t.replace('#include "../capture/live_alias.h"', '''#include "../capture/live_alias.h"
/* Native file calls only accept the original mapping: map a cacheable alias back (same PA). */
static void *la_native_va(const void *p){uintptr_t a=(uintptr_t)p;
 return (void *)(a>=UINT64_C(0xc00000000)&&a<UINT64_C(0x1000000000)?a-UINT64_C(0x400000000):a);}''', 1)
F.write_text(t)
H = src / 'capture/live_stream.c'; h = H.read_text()
h = h.replace('s->diag.version=5;', 's->diag.version=7;'); H.write_text(h)
print('io patch applied to', src)
