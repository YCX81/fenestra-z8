#!/usr/bin/env python3
"""Live writer: 640x424 previews (LVP). Usage: preview_hires_patch.py SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new):
    p = src / path; t = p.read_text(); assert t.count(old) == 1, (path, old); p.write_text(t.replace(old, new))
sub('capture/live_stream.h', '#define LIVE_PREVIEW_W 320u', '#define LIVE_PREVIEW_W 640u')
sub('capture/live_stream.h', '#define LIVE_PREVIEW_H 212u', '#define LIVE_PREVIEW_H 424u')
sub('codec/preview_writer.h', '#define LIVE_PREVIEW_WIDTH 320u', '#define LIVE_PREVIEW_WIDTH 640u')
sub('codec/preview_writer.h', '#define LIVE_PREVIEW_HEIGHT 212u', '#define LIVE_PREVIEW_HEIGHT 424u')
sub('codec/preview_writer.h', '#define LIVE_PREVIEW_FRAME_BYTES (320u * 212u * 3u / 2u)', '#define LIVE_PREVIEW_FRAME_BYTES (640u * 424u * 3u / 2u)')
print('preview hires patch applied to', src)
