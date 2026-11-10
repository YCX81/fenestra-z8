#!/usr/bin/env python3
"""v31 on top of v30 (never flashed): fixes from the pre-flash review.
- Picture completion in w5_close: wait for a NEW frame-info record in any of the 9 entries (stamp or address changed,
  size set). hw_index can lag the service after a frame error, and stamp 0 matches cleared entries.
- open_stream refuses after a failed close (init reopened otherwise).
- Preempt: wait up to 4 s for the in-flight call; if it is still busy, do not close concurrently (stage 10).
- Lock timeout in w5_close counts as a failure (stage 11). Reuse path of live_encoder_init holds busy.
Not fixable here: after a failed hand-back the stock bring-up still runs (the hook cannot refuse it).
Usage: v31_patch.py SRC_DIR (a copy of src-fix30)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
W = 'codec/live_w5enc.c'
def sub(old, new, count=1):
    p = src / W; t = p.read_text(); assert t.count(old) == count, (old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    sub('''    if (TRIGGER(s, 0, k, 0, 0)) return 4;
    volatile uint8_t *info = INFO(s, w->hw_index % 9);
    uint32_t stamp = w->hw_index * 3000u;
    uint64_t t = now_us();
    while (*(volatile uint32_t *)(info + 0x60) != stamp) { if (now_us() - t > 500000u) return 5; DELAY(1); }
''', '''    uint32_t st0[9]; uint64_t ad0[9];
    for (unsigned i = 0; i < 9; i++) { st0[i] = *(volatile uint32_t *)(INFO(s, i) + 0x60); ad0[i] = *(volatile uint64_t *)INFO(s, i); }
    if (TRIGGER(s, 0, k, 0, 0)) return 4;
    uint64_t t = now_us();
    for (unsigned done = 0; !done; ) {
        for (unsigned i = 0; i < 9; i++) {
            volatile uint8_t *e = INFO(s, i);
            if (*(volatile uint32_t *)(e + 8) && (*(volatile uint32_t *)(e + 0x60) != st0[i] || *(volatile uint64_t *)e != ad0[i])) done = 1;
        }
        if (!done) { if (now_us() - t > 500000u) return 5; DELAY(1); }
    }
''')
    sub('        free_ = 0; if (now_us() - t > 4000000u) return; DELAY(1);\n',
        '        free_ = 0;\n        if (now_us() - t > 4000000u) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | 11u << 24; return; }\n        DELAY(1);\n')
    sub('    if (VDI_BASE) return LIVE_ENCODER_BUSY;\n', '    if (w->close_failed) return LIVE_ENCODER_BUSY;   /* v31 */\n    if (VDI_BASE) return LIVE_ENCODER_BUSY;\n')
    sub('now_us() - start < W5_PREEMPT_WAIT_US) DELAY(1);\n    if (alive(w)) w5_close(w);',
        'now_us() - start < 4000000u) DELAY(1);\n'
        '    if (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST)) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | 10u << 24; }   /* v31: never close under a running call */\n'
        '    else if (alive(w)) w5_close(w);')
    sub('''        if (w->magic != W5_MAGIC) return -1;
        if (w->width != width || w->height != height) {
''', '''        if (w->magic != W5_MAGIC) return -1;
        __atomic_store_n(&w->busy, 1, __ATOMIC_SEQ_CST);   /* v31: preempt waits for this path */
        if (w->width != width || w->height != height) {
''')
    sub('            if ((size_t)width * ((height + 31) & ~31u) * 3 / 2 > w->slot_bytes) return -1;\n',
        '            if ((size_t)width * ((height + 31) & ~31u) * 3 / 2 > w->slot_bytes) { __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST); return -1; }\n')
    sub('            if (rc && rc != LIVE_ENCODER_BUSY) { w->magic = 0; return -1; }',
        '            if (rc && rc != LIVE_ENCODER_BUSY) { w->magic = 0; __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST); return -1; }')
    sub('        w->frames = 0; w->batch_open = 0;\n        return 0;\n    }\n    void *ad, *bd;',
        '        w->frames = 0; w->batch_open = 0;\n        __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST);\n        return 0;\n    }\n    void *ad, *bd;')
    print('v31 review fixes applied to', src)
