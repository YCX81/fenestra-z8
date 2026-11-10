#!/usr/bin/env python3
"""v21 on top of v20: portrait Live preview fills the LVP frame.
Hardware 11-08: in camera playback a portrait still shows correctly but its Live looks squeezed with black bars. The
playback UI scales each LVP frame into the still's on-screen rectangle (native_ui.c: OpenVG matrix ratio(right-left,
width), ratio(top-bottom, height)), which for a portrait still is portrait-shaped; the v13 preview had already fitted the
rotated picture 452x682 with bars inside the 1024x682 frame, so it was squeezed a second time. Now the rotated picture
is stretched to the whole 1024x682 frame (anisotropic), and the UI's rectangle restores its proportions. MOV unchanged.
Usage: v21_patch.py SRC_DIR (a copy of src-fix20)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    sub('capture/live_stream.c', ' enum{W=LIVE_PREVIEW_W,H=LIVE_PREVIEW_H,CW=(H*H/W)&~3u,X0=((W-CW)/2)&~1u};',
        ' enum{W=LIVE_PREVIEW_W,H=LIVE_PREVIEW_H,CW=W,X0=0}; /* v21: fill the frame; playback scales it into the portrait rectangle */')
    print('v21 portrait preview fill applied to', src)
