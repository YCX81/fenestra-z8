#!/usr/bin/env python3
"""Live writer: 1024x682 previews (LVP). Usage: preview_hires_patch.py SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new):
    p = src / path; t = p.read_text(); assert t.count(old) == 1, (path, old); p.write_text(t.replace(old, new))
sub('capture/live_stream.h', '#define LIVE_PREVIEW_W 320u', '#define LIVE_PREVIEW_W 1024u')
sub('capture/live_stream.h', '#define LIVE_PREVIEW_H 212u', '#define LIVE_PREVIEW_H 682u')
sub('codec/preview_writer.h', '#define LIVE_PREVIEW_WIDTH 320u', '#define LIVE_PREVIEW_WIDTH 1024u')
sub('codec/preview_writer.h', '#define LIVE_PREVIEW_HEIGHT 212u', '#define LIVE_PREVIEW_HEIGHT 682u')
sub('codec/preview_writer.h', '#define LIVE_PREVIEW_FRAME_BYTES (320u * 212u * 3u / 2u)', '#define LIVE_PREVIEW_FRAME_BYTES (1024u * 682u * 3u / 2u)')
sub('capture/live_stream.h', '#define LIVE_PREVIEW_SLOTS 64u', '#define LIVE_PREVIEW_SLOTS 50u   /* 3.3 s at 15 fps, about 52 MB */')
print('preview full-resolution patch applied to', src)
