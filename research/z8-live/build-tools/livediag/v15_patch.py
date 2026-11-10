#!/usr/bin/env python3
"""v15 on top of v14: Wave5 Live encoder at 12 Mbps instead of 8.
The Wave5 bitrate is fixed when the encoder opens (W5_KBPS: bring-up, CONFIGURE and the mode-table bps);
the per-frame bitrate argument is ignored, so stream.bitrate set over USB had no effect (hardware 11-07:
DSC_9766 at 'rate 12000000' came out at 5.2 Mbps). The rate control assumes 29.97 fps while Live feeds
~26 fps, so 8 Mbps gave ~6 Mbps; 12 Mbps should give ~9 Mbps, ~3.4 MB per 3 s clip (ring 5 MB).
Usage: v15_patch.py SRC_DIR (a copy of src-fix14)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old, t.count(old)); p.write_text(t.replace(old, new))
sub('codec/live_w5enc.c', '#define W5_KBPS 8000u', '#define W5_KBPS 12000u')
sub('capture/live_stream.h', '#define LIVE_BITRATE 8000000u', '#define LIVE_BITRATE 12000000u')
print('v15 bitrate patch applied to', src)
