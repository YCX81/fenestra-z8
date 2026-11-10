#!/usr/bin/env python3
"""v18 on top of v17: retry a duplicate pick within the same frame.
Hardware 11-08 (v17 clips DSC_9767/9768 vs v13.1 DSC_9764): at the live-view interrupt the hardware has not always
moved its 'current buffer' word yet, so the picker returns the buffer already kept (duplicate) and the next try
comes a whole frame later, when the source has moved two buffers: 'advance 0' and 'advance 2' pairs
(9764: 23/73/25 of 0/1/2, 9768: 27/54/30) -> a repeated-then-skipped picture, visible judder. Now a duplicate
leaves the frame unconsumed (last_generation = seq-2) and the worker (6 ms loop) tries the same frame again,
up to 3 times; only then is it dropped as a duplicate. The retry state reuses stream.preview_pad (unused):
bits 4..31 = low bits of the frame's seq, bits 0..3 = retries so far.
Usage: v18_patch.py SRC_DIR (a copy of src-fix17)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    sub('capture/live_stream.c',
        '  if(s->last_kept_y&&D->y==s->last_kept_y){c->ops.lease_end(c->ops.context,&source);s->dup_skipped++;fr.flags|=4;c->dropped++;goto finish;}',
        '  if(s->last_kept_y&&D->y==s->last_kept_y){\n'
        '   c->ops.lease_end(c->ops.context,&source);fr.flags|=4;\n'
        '   /* v18: the current-buffer word may move a few ms after the interrupt: try this frame again */\n'
        '   uint32_t key=(uint32_t)(seq<<4),tries=(s->preview_pad&~15u)==key?(s->preview_pad&15u):0u;\n'
        '   if(tries<3u){s->preview_pad=key|(tries+1u);c->last_generation=seq-2u;goto finish;}\n'
        '   s->dup_skipped++;c->dropped++;goto finish;}')
    print('v18 duplicate retry applied to', src)
