#!/usr/bin/env python3
"""v25 on top of v23 (v24 dropped): Wave5 service bring-up with the original 8000 kbps.
Hardware 11-08: v24 (free Live's Wave5 pool blocks when leaving photo mode) made every switch to movie mode crash at
once - the Wave5 service keeps using the arena Live handed to BRINGUP, so it must not be freed. The REC hang
(movie REC right after Live had run, even with Live off; never after a fresh boot with Live off) became frequent
from v15, which passed W5_KBPS = 12000 to BRINGUP as well as to the encoder. The service sizes its state from the
bring-up rate and the movie client then inherits it. Now BRINGUP gets 8000 again (as up to v14); CONFIGURE and the
mode table keep 12000 so Live clips stay at the v15 bitrate.
Usage: v25_patch.py SRC_DIR (a copy of src-fix23)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    sub('codec/live_w5enc.c', '    if (BRINGUP(W5_CLIENT, W5_KBPS, w->arena, W5_ARENA) != 1) return -3;',
        '    if (BRINGUP(W5_CLIENT, 8000u /* v25: service bring-up rate as up to v14 */, w->arena, W5_ARENA) != 1) return -3;')
    print('v25 bring-up rate applied to', src)
