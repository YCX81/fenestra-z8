#!/usr/bin/env python3
"""v20 on top of v19: default minimum frame spacing 29 ms (was LA_SAMPLE_INTERVAL_US = 25 ms).
Hardware 11-08: live view runs at 120 fps before the half press and 30 fps during it; with 25 ms the clip mixed
25 ms and 33 ms steps (DSC_9770: 34.5 fps, visible judder). With sample_us = 29000 set over USB, DSC_9774 came out
at 29.0 fps, 83 of 88 steps at 33 ms, 0 torn. 29 ms keeps every 30 fps frame (33.3 ms with jitter) and gives
33.3 ms steps from 60/120 fps sources.
Usage: v20_patch.py SRC_DIR (a copy of src-fix19)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    sub('capture/live_stream.c', 'LA_WINDOW_US,LA_SAMPLE_INTERVAL_US,LA_MAX_GAP_US,', 'LA_WINDOW_US,29000u /* v20 */,LA_MAX_GAP_US,')
    print('v20 sample spacing applied to', src)

# 2. Portrait preview aliasing (hardware 11-08: 'a vertical pole shows mosaic-like patterns' in camera playback; the MOV
#    itself is clean): the v13 rotate-and-fit of each LVP frame (scale 0.66) used nearest-neighbour samples, so thin
#    lines broke into jaggies. Now each output sample is bilinear (8-bit fractions) from the 2x2 source neighbourhood.
OLD = ('   unsigned ry=y*w/h; /* rotated image is h wide, w tall: row y of the fit maps to rotated row ry */\n'
       '   for(unsigned x=0;x<w;x++){\n'
       '    if(x<x0||x>=x0+cw){row[x]=fill;continue;}\n'
       '    unsigned rx=(x-x0)*h/cw;if(rx>=h)rx=h-1;if(ry>=w)ry=w-1;\n'
       '    unsigned sx=o==6?ry:w-1-ry,syy=o==6?h-1-rx:rx;row[x]=sp[(size_t)syy*w+sx];}}}\n')
NEW = ('   uint32_t ryf=(uint32_t)(((uint64_t)y*w<<8)/h),mx=(w-1u)<<8,my=(h-1u)<<8; /* v20: 24.8 fixed point */\n'
       '   if(ryf>mx)ryf=mx;\n'
       '   for(unsigned x=0;x<w;x++){\n'
       '    if(x<x0||x>=x0+cw){row[x]=fill;continue;}\n'
       '    uint32_t rxf=(uint32_t)(((uint64_t)(x-x0)*h<<8)/cw);if(rxf>my)rxf=my;\n'
       '    uint32_t sxf=o==6?ryf:mx-ryf,syf=o==6?my-rxf:rxf;\n'
       '    unsigned ix=sxf>>8,iy=syf>>8,fx=sxf&255u,fy=syf&255u,ix1=ix+1u<w?ix+1u:ix,iy1=iy+1u<h?iy+1u:iy;\n'
       '    const uint8_t *r0=sp+(size_t)iy*w,*r1=sp+(size_t)iy1*w;\n'
       '    uint32_t top=r0[ix]*(256u-fx)+r0[ix1]*fx,bot=r1[ix]*(256u-fx)+r1[ix1]*fx;\n'
       '    row[x]=(uint8_t)((top*(256u-fy)+bot*fy+32768u)>>16);}}}\n')
if __name__ == '__main__':
    sub('capture/live_stream.c', OLD, NEW)
    print('v20 bilinear portrait preview applied to', src)
