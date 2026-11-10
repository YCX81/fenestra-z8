#!/usr/bin/env python3
"""v22 on top of v20: Live must not take Wave5 back right after a movie/playback client took it.
Hardware 11-08: switching from photo (Live running) to movie and pressing REC at once hung the camera (log: Wave5
hand-backs to client 2 at 23.81 s and 24.07 s, then freeze); after the reboot REC worked. The preempt (stock bring-up
hook) closes Live's stream, but a frame still in the photo->movie transition reached frame_body, found the stream
closed and reopened it (BRINGUP of Live's client) while the movie client was bringing Wave5 up. Now a preempt by
another client blocks Live's reopen for 3 s; the encoder returns LIVE_ENCODER_BUSY meanwhile (frame dropped).
Usage: v22_patch.py SRC_DIR (a copy of src-fix20)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    W = 'codec/live_w5enc.c'
    sub(W, '    uint64_t t_in, t_wait, t_out, t_parse, s_in, s_wait, s_out, s_parse, n_timed;   /* diag, us */',
        '    uint64_t t_in, t_wait, t_out, t_parse, s_in, s_wait, s_out, s_parse, n_timed;   /* diag, us */\n'
        '    uint64_t preempt_us; uint32_t reopen_blocked, pad22;   /* v22: no reopen within 3 s of a preempt */')
    sub(W, '    w->frames = 0; w->batch_open = 0; w->preempts++;\n',
        '    w->frames = 0; w->batch_open = 0; w->preempts++; w->preempt_us = now_us();\n')
    sub(W, '    if (!alive(w)) {\n        int rc = open_stream(w);\n        if (rc == LIVE_ENCODER_BUSY) return rc;',
        '    if (!alive(w)) {\n'
        '        /* v22: a movie/playback client just took Wave5: do not take it back during its bring-up */\n'
        '        if (w->preempt_us && now_us() - w->preempt_us < 3000000u) { w->reopen_blocked++; return LIVE_ENCODER_BUSY; }\n'
        '        int rc = open_stream(w);\n        if (rc == LIVE_ENCODER_BUSY) return rc;')
    print('v22 Wave5 reopen cooldown applied to', src)
