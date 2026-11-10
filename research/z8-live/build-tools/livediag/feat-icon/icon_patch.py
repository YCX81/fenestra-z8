#!/usr/bin/env python3
"""Playback badge: replace the dot-matrix LIVE with the approved concentric-ring icon (icons-live/live-112x44.png,
white on black, 8-bit coverage). Usage: icon_patch.py PB_DIR PNG (PB_DIR = copy of pbfix/live-playback-ui-v28)"""
import re, sys
from pathlib import Path
from PIL import Image
pb, png = Path(sys.argv[1]), sys.argv[2]
im = Image.open(png).convert('L'); assert im.size == (112, 44)
d = im.tobytes()
rows = ',\n'.join(','.join(map(str, d[i:i + 28])) for i in range(0, len(d), 28))
(pb / 'live_badge.h').write_text('static const uint8_t pb_live_badge[112u*44u]={\n' + rows + '\n};\n')
u = pb / 'ui.c'; t = u.read_text()
t, n = re.subn(r'\n[^\n]*letters\[[^\n]*(?:\n[^\n]*\{[\d,]+\}[^\n]*)*?;\n', '\n', t, count=1); assert n == 1, 'letters table'
old = re.search(r'            uint8_t color=0u;\n            if\(row>=8u&&row<36u&&col>=8u\) \{.*?\n            \}\n', t, re.S); assert old
t = t.replace(old.group(0), '            uint8_t color=pb_live_badge[row*PB_BADGE_WIDTH+col];\n')
t = t.replace('#include "ui.h"\n', '#include "ui.h"\n#include "live_badge.h"\n', 1)
u.write_text(t); print('badge patched,', sum(1 for b in d if b > 128), 'bright pixels')
