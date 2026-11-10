#!/usr/bin/env python3
"""v30 on top of v29: the Wave5 hand-back proven on the camera 11-08 (w5seq.py; movie REC worked after it).
STOP, TRIGGER slot k (last normal picture), wait for that picture, TRIGGER slot k+1 (drain: source index -1), wait
worker state 5, FINISH, check worker 0 and the instance freed, EXIT, wait VDI 0. v29 triggered once, so the stream
never ended (fin_rc 0x01040400). 0xa02d31070 is only a latch set before the submit, and FINISH's rc is cleared before
it returns, so neither is evidence. Any failed check stops the sequence (no blind FINISH/EXIT) and leaves
close_failed set: no more encoding or retries on that stream until reboot. A lock serialises worker and preempt
closes; open_stream refuses outside photo mode, so a stale step cannot bring Wave5 up during movie mode.
fin_rc = failed stage << 24 | worker before finish << 8 | worker after << 16 (stage 0 = success).
Usage: v30_patch.py SRC_DIR (a copy of src-fix29)"""
import re, sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
NEW = r'''static int w5_close_body(struct live_w5 *w) {
    uint32_t s = w->stream;
    volatile uint32_t *inst = (volatile uint32_t *)(uintptr_t)*(volatile uint64_t *)(uintptr_t)(UINT64_C(0xa02d32758) + s * 8u);
    if (WSTATE(s) != 4u || !inst || inst[0] != 1u || inst[3] != 3u) return 1;
    unsigned k = w->frames % W5_SLOTS;
    if (!k) {
        uint8_t *list = w->scratch + 0x800;
        for (unsigned i = 0; i < W5_SLOTS; i++) {
            put64(list + i * 16, w->slot[i]);
            put64(list + i * 16 + 8, w->slot[i] + (uint64_t)w->width * ((w->height + 31) & ~31u));
        }
        if (SUBMIT(s, 0, list)) return 2;
    }
    if (W5_STOP(s)) return 3;
    if (TRIGGER(s, 0, k, 0, 0)) return 4;
    volatile uint8_t *info = INFO(s, w->hw_index % 9);
    uint32_t stamp = w->hw_index * 3000u;
    uint64_t t = now_us();
    while (*(volatile uint32_t *)(info + 0x60) != stamp) { if (now_us() - t > 500000u) return 5; DELAY(1); }
    w->hw_index++; w->frames++;
    if (TRIGGER(s, 0, (k + 1) % W5_SLOTS, 0, 0)) return 6;   /* drain */
    t = now_us();
    while (WSTATE(s) != 5u) { if (now_us() - t > 1000000u) return 7; DELAY(1); }
    w->fin_rc = 5u << 8;
    W5_FINISH(s);
    w->fin_rc |= (WSTATE(s) & 0xffu) << 16;
    if (WSTATE(s) != 0u || inst[0] != 0u) return 8;
    EXIT(W5_CLIENT);
    t = now_us();
    while (VDI_BASE) { if (now_us() - t > 1000000u) return 9; DELAY(1); }
    return 0;
}
static void w5_close(struct live_w5 *w) {
    uint32_t free_ = 0; uint64_t t = now_us();
    while (!__atomic_compare_exchange_n(&w->close_lock, &free_, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        free_ = 0; if (now_us() - t > 4000000u) return; DELAY(1);
    }
    if (alive(w) && !w->close_failed) {
        int st = w5_close_body(w);
        if (st) { w->close_failed = 1; w->eos_miss++; w->fin_rc = (w->fin_rc & 0xffffffu) | (uint32_t)st << 24; } else w->eos_ok++;
        w->closes++; w->last_close_us = (uint32_t)(now_us() - t);
        if (!st) { w->frames = 0; w->batch_open = 0; }
    }
    __atomic_store_n(&w->close_lock, 0, __ATOMIC_RELEASE);
}
'''
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    W = 'codec/live_w5enc.c'
    p = src / W; t = p.read_text()
    a = t.index('static void w5_close(struct live_w5 *w) {'); b = t.index('/* v27: worker task, Live not encoding')
    p.write_text(t[:a] + NEW + t[b:])
    sub(W, '    uint32_t eos_ok, eos_miss, fin_rc, last_close_us;   /* v27 */',
        '    uint32_t eos_ok, eos_miss, fin_rc, last_close_us;   /* v27 */\n    uint32_t close_lock, close_failed;   /* v30 */')
    sub(W, '    if (VDI_BASE) return LIVE_ENCODER_BUSY;\n',
        '    if (VDI_BASE) return LIVE_ENCODER_BUSY;\n'
        '    if (RD32(0xa03bd062c) != 0) return LIVE_ENCODER_BUSY;   /* v30: bring-up only in photo mode */\n')
    sub(W, 'unsigned interval, const uint8_t **out, size_t *bytes, unsigned *idr) {\n    if (!alive(w)) {',
        'unsigned interval, const uint8_t **out, size_t *bytes, unsigned *idr) {\n'
        '    if (w->close_failed) return LIVE_ENCODER_BUSY;   /* v30 */\n    if (!alive(w)) {')
    print('v30 proven Wave5 hand-back applied to', src)
