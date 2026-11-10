#!/usr/bin/env python3
"""v29 on top of v28: wait for the Wave5 service between the close steps; short audio history is padded.
1) Hardware 11-08 (v28, movie mode, USB): Live's worker ran w5_close (closes 1, eos_ok 1, fin 0) in 2.4 ms but the
   service stayed up: instance state 3, worker state 4, deinit -1 -> REC still hangs. In the USB experiment the steps
   were ~0.1 s apart and the worker state went 4 -> 5 (EOS handled) -> 0 (finish). The EOS done flag is set before
   the worker reaches state 5, and finish in state 4 is ignored. Now: after the EOS picture wait for worker state 5
   (0xa02d3274c[s]), after finish wait for it to leave 5, after exit wait for the VDI to go down (bounded, 0.5 s each).
   fin_rc keeps rc | worker state before finish << 8 | after << 16 | VDI up after exit << 24.
2) Hardware 11-08: a Live shot 0.8 s after the previous clip was written had no sound: audio history restarts
   when the previous clip is released, so the 1.5 s before the press were not all recorded and the freeze gave up
   (phase 3). Now freeze accepts a window that starts before the recorded history and the missing start is silence.
Usage: v29_patch.py SRC_DIR (a copy of src-fix28)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    W = 'codec/live_w5enc.c'
    sub(W, '#define EOS_DONE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d31070) + (s) * 4u))\n',
        '#define EOS_DONE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d31070) + (s) * 4u))\n'
        '#define WSTATE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d3274c) + (s) * 4u))   /* v29: 4 run, 5 EOS, 0 free */\n')
    sub(W, '            while (!EOS_DONE(s) && now_us() - t < 500000u) DELAY(1);\n',
        '            while (!EOS_DONE(s) && now_us() - t < 500000u) DELAY(1);\n'
        '            uint64_t t5 = now_us();\n'
        '            while (WSTATE(s) != 5u && now_us() - t5 < 500000u) DELAY(1);   /* v29 */\n')
    sub(W, '    w->fin_rc = W5_FINISH(s);\n    EXIT(W5_CLIENT);\n',
        '    uint32_t before = WSTATE(s);\n'
        '    uint32_t rc = W5_FINISH(s);\n'
        '    uint64_t tf = now_us();\n'
        '    while (WSTATE(s) == 5u && now_us() - tf < 500000u) DELAY(1);   /* v29 */\n'
        '    uint32_t after = WSTATE(s);\n'
        '    EXIT(W5_CLIENT);\n'
        '    uint64_t te = now_us();\n'
        '    while (VDI_BASE && now_us() - te < 500000u) DELAY(1);   /* v29 */\n'
        '    w->fin_rc = (rc & 0xffu) | (before & 0xffu) << 8 | (after & 0xffu) << 16 | (VDI_BASE ? 1u : 0u) << 24;\n')
    A = 'audio/live_audio.c'
    sub(A, ' if(!a->count || lo<a->first_index || hi>a->next_index || hi<=lo)return LAUDIO_GAP;',
        ' if(!a->count || hi>a->next_index || hi<=lo || a->first_index>=hi)return LAUDIO_GAP; /* v29: a start before the history is silence */')
    sub(A, '  for(unsigned ch=0;ch<2;ch++)out[2*i+ch]=(int16_t)((int32_t)a->ring[slot+ch]*(int32_t)g/32768);',
        '  for(unsigned ch=0;ch<2;ch++)out[2*i+ch]=index<a->first_index?0:(int16_t)((int32_t)a->ring[slot+ch]*(int32_t)g/32768);')
    print('v29 Wave5 close waits + audio padding applied to', src)
