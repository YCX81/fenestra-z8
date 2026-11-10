#!/usr/bin/env python3
"""v32 on top of v31: no Wave5 close at the photo->movie switch; only the bring-up hook hands Wave5 back.
Hardware 11-08 on v31: the worker's close at the mode switch rebooted the camera. With the worker close held off from
USB (release_req=1), the switch survived with live view running and movie REC worked, the hand-back then running in
the REC bring-up hook. The playback badge is the approved ring icon (PB_SRC=pbfix/live-playback-ui-icon).
Usage: v32_patch.py SRC_DIR (a copy of src-fix31)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
p = src / 'runtime.c'; t = p.read_text()
old = ("  /* v27: not in photo mode and no clip in flight: close Live's Wave5 stream the stock way */\n"
       "  if(!photo&&!LOAD(&r->stream.job)&&r->stream.encoder.hw){\n"
       "   int rel=live_encoder_release(&r->stream.encoder);\n"
       "   if(rel)crumb(r,now_us(),EV_W5,0xf0u|(uint32_t)(rel&0xf),0);}\n")
assert t.count(old) == 1
p.write_text(t.replace(old, '  /* v32: no close at the mode switch; the bring-up hook hands Wave5 back */\n'))
print('v32 applied to', src)
