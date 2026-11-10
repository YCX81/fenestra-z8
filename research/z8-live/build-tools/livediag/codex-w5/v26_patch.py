#!/usr/bin/env python3
"""v26 on top of v25: close Live's Wave5 stream the way the stock recorder does before the stock exit.
Hardware 11-08 (v25): movie REC hangs once Live has run this boot, also with Live audio off. RAM dump in movie
mode before REC: Live (client 4) still holds stream 0 (VDI up on Live's arena), so the REC bring-up runs the
preempt hook, which called the stock exit 0x402f3084 on a stream that is still RUNning. The stock recorder never
does that: its encoder task (0x402f4c54 / 0x402f4d10) first requests end of stream 0x40c34078(stream), waits for
callback type 3 (0x402f64f8 sets the stop-done event), then finishes 0x40c340e8(stream) (worker event 0x400,
bounded wait for 0x800), and only then exits (0x40d0712c). Same reason the stock exit hung the camera in photo
mode. Now every Live exit (preempt hook, channel-size reopen) runs stop -> wait type 3 (<= 300 ms) -> finish ->
exit. Live's stream callback records the event types (was a no-op) for the wait.
Usage: v26_patch.py SRC_DIR (a copy of src-fix25)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    W = 'codec/live_w5enc.c'
    sub(W, '    uint64_t preempt_us; uint32_t reopen_blocked, pad22;   /* v22: no reopen within 3 s of a preempt */',
        '    uint64_t preempt_us; uint32_t reopen_blocked, cb_mask;   /* v22 cooldown; v26 stream callback types seen */\n'
        '    uint32_t eos_ok, eos_miss, eos_wait_us, closes;   /* v26 */')
    sub(W, '    if (OPEN(stream, NOOP_CB)) return -5;',
        '    if (OPEN(stream, (uint64_t)(uintptr_t)w5_cb)) return -5;   /* v26: was NOOP_CB */')
    sub(W, 'static int open_stream(struct live_w5 *w) {',
        '/* v26: stream callback (service worker task): note the event type; 3 = end of stream done */\n'
        'void live_w5_event(uint32_t type);\n'
        'static uint32_t w5_cb(uint32_t stream, uint32_t type, void *info) { (void)stream; (void)info; live_w5_event(type & 0xffu); return 0; }\n'
        'void live_encoder_event(live_encoder *e, uint32_t type) {\n'
        '    struct live_w5 *w = e ? e->hw : 0;\n'
        '    if (w && w->magic == W5_MAGIC && type < 32) __atomic_or_fetch(&w->cb_mask, 1u << type, __ATOMIC_SEQ_CST);\n'
        '}\n'
        '#define W5_STOP FN(0x40c34078, uint32_t (*)(uint32_t))     /* end-of-stream request */\n'
        '#define W5_FINISH FN(0x40c340e8, uint32_t (*)(uint32_t))   /* worker 0x400 -> wait 0x800 (bounded) */\n'
        '/* v26: stock order before the exit: stop, wait for callback 3, finish; then exit */\n'
        'static void w5_close(struct live_w5 *w) {\n'
        '    if (alive(w)) {\n'
        '        uint32_t s = w->stream;\n'
        '        __atomic_and_fetch(&w->cb_mask, ~8u, __ATOMIC_SEQ_CST);\n'
        '        uint64_t t = now_us();\n'
        '        if (!W5_STOP(s)) {\n'
        '            while (!(__atomic_load_n(&w->cb_mask, __ATOMIC_SEQ_CST) & 8u) && now_us() - t < 300000u) DELAY(1);\n'
        '        }\n'
        '        if (__atomic_load_n(&w->cb_mask, __ATOMIC_SEQ_CST) & 8u) w->eos_ok++; else w->eos_miss++;\n'
        '        w->eos_wait_us = (uint32_t)(now_us() - t);\n'
        '        W5_FINISH(s);\n'
        '    }\n'
        '    w->closes++;\n'
        '    EXIT(W5_CLIENT);\n'
        '}\n\n'
        'static int open_stream(struct live_w5 *w) {')
    sub(W, '            if (alive(w)) EXIT(W5_CLIENT);\n            w->width = width;',
        '            if (alive(w)) w5_close(w);\n            w->width = width;')
    sub(W, '    while (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST) && now_us() - start < W5_PREEMPT_WAIT_US) DELAY(1);\n    EXIT(W5_CLIENT);',
        '    while (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST) && now_us() - start < W5_PREEMPT_WAIT_US) DELAY(1);\n    w5_close(w);')
    sub('codec/live_codec.h', 'void live_encoder_preempt(live_encoder *,uint32_t client);',
        'void live_encoder_preempt(live_encoder *,uint32_t client);\nvoid live_encoder_event(live_encoder *,uint32_t type); /* v26 */')
    sub('runtime.c', ' return r && r->magic==LIVE_RUNTIME_MAGIC && r->size==sizeof(*r)?r:0;\n}\n',
        ' return r && r->magic==LIVE_RUNTIME_MAGIC && r->size==sizeof(*r)?r:0;\n}\n'
        '/* v26: Wave5 stream callback, any task */\n'
        'void live_w5_event(uint32_t type){struct live_runtime *r=state();if(r)live_encoder_event(&r->stream.encoder,type);}\n')
    print('v26 Wave5 stop/finish before exit applied to', src)
