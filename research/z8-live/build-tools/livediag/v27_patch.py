#!/usr/bin/env python3
"""v27 on top of v25 (v26 dropped): hand Wave5 back the way the stock recorder ends a clip.
Hardware 11-08 (v25, USB experiments w5drain.py): the stock exit 0x402f3084 alone leaves the service up on Live's
arena (instance state 3 is not closable: 0x40c46b44 accepts only 1/0x21, so 0x40c453ec skips the close, worker
deinit returns -1, VDI stays up) -> movie REC's bring-up/open is refused (arena differs) and the recorder runs on
with stream 0xff -> hang. Completing the open batch alone did not help. What worked on the camera:
stop 0x40c34078(s) (EOS request) -> trigger one more picture (EOS is processed at a picture completion; done flag
0xa02d31070[s]) -> finish 0x40c340e8(s) (instance freed, worker state 0) -> exit 0x402f3084(4) -> VDI 0; movie REC
then worked. v26's stream callback made the camera stutter and reboot-loop at boot: callback stays NOOP_CB.
Now: w5_close() does that order; Live's worker calls it as soon as the camera is not in photo mode and no clip
is in flight (so REC never has to preempt a running stream); the preempt hook and the channel-size reopen use it
as well. Memory stays allocated (service keeps the arena pointer); the next photo-mode frame brings Wave5 up again.
Usage: v27_patch.py SRC_DIR (a copy of src-fix25)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    W = 'codec/live_w5enc.c'
    sub(W, '    uint64_t preempt_us; uint32_t reopen_blocked, pad22;   /* v22: no reopen within 3 s of a preempt */',
        '    uint64_t preempt_us; uint32_t reopen_blocked, closes;   /* v22 cooldown; v27 clean closes */\n'
        '    uint32_t eos_ok, eos_miss, fin_rc, last_close_us;   /* v27 */')
    sub(W, 'static int open_stream(struct live_w5 *w) {',
        '#define W5_STOP FN(0x40c34078, uint32_t (*)(uint32_t))     /* EOS request, handled at the next picture */\n'
        '#define W5_FINISH FN(0x40c340e8, uint32_t (*)(uint32_t))   /* frees the instance (worker 0x400/0x800) */\n'
        '#define EOS_DONE(s) (*(volatile uint32_t *)(uintptr_t)(UINT64_C(0xa02d31070) + (s) * 4u))\n'
        '/* v27: stock end-of-clip order, then the stock exit (verified on the camera 11-08). No frame in flight. */\n'
        'static void w5_close(struct live_w5 *w) {\n'
        '    if (!alive(w)) return;\n'
        '    uint32_t s = w->stream; uint64_t t = now_us();\n'
        '    if (!W5_STOP(s)) {\n'
        '        unsigned slot = w->frames % W5_SLOTS, ok = 1;\n'
        '        if (!slot) {\n'
        '            uint8_t *list = w->scratch + 0x800;\n'
        '            for (unsigned i = 0; i < W5_SLOTS; i++) {\n'
        '                put64(list + i * 16, w->slot[i]);\n'
        '                put64(list + i * 16 + 8, w->slot[i] + (uint64_t)w->width * ((w->height + 31) & ~31u));\n'
        '            }\n'
        '            ok = !SUBMIT(s, 0, list);\n'
        '        }\n'
        '        if (ok && !TRIGGER(s, 0, slot, 0, 0)) {\n'
        '            while (!EOS_DONE(s) && now_us() - t < 500000u) DELAY(1);\n'
        '            w->hw_index++; w->frames++;\n'
        '        }\n'
        '    }\n'
        '    if (EOS_DONE(s)) w->eos_ok++; else w->eos_miss++;\n'
        '    w->fin_rc = W5_FINISH(s);\n'
        '    EXIT(W5_CLIENT);\n'
        '    w->closes++; w->last_close_us = (uint32_t)(now_us() - t);\n'
        '    w->frames = 0; w->batch_open = 0;\n'
        '}\n'
        '/* v27: worker task, Live not encoding (not in photo mode, no clip in flight): give Wave5 back now */\n'
        'int live_encoder_release(live_encoder *e) {\n'
        '    struct live_w5 *w = e ? e->hw : 0;\n'
        '    if (!w || w->magic != W5_MAGIC || !alive(w)) return 0;\n'
        '    if (__atomic_load_n(&w->release_req, __ATOMIC_SEQ_CST)) return 0;   /* a preempt is doing it */\n'
        '    __atomic_store_n(&w->busy, 1, __ATOMIC_SEQ_CST);\n'
        '    if (__atomic_load_n(&w->release_req, __ATOMIC_SEQ_CST)) { __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST); return 0; }\n'
        '    w5_close(w);\n'
        '    __atomic_store_n(&w->busy, 0, __ATOMIC_SEQ_CST);\n'
        '    return alive(w) ? -1 : (VDI_BASE ? 2 : 1);\n'
        '}\n\n'
        'static int open_stream(struct live_w5 *w) {')
    sub(W, '            if (alive(w)) EXIT(W5_CLIENT);\n            w->width = width;',
        '            if (alive(w)) w5_close(w);\n            w->width = width;')
    sub(W, '    while (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST) && now_us() - start < W5_PREEMPT_WAIT_US) DELAY(1);\n    EXIT(W5_CLIENT);',
        '    while (__atomic_load_n(&w->busy, __ATOMIC_SEQ_CST) && now_us() - start < W5_PREEMPT_WAIT_US) DELAY(1);\n'
        '    if (alive(w)) w5_close(w);   /* v27: the worker may have released it meanwhile */')
    sub('codec/live_codec.h', 'void live_encoder_preempt(live_encoder *,uint32_t client);',
        'void live_encoder_preempt(live_encoder *,uint32_t client);\nint live_encoder_release(live_encoder *); /* v27 */')
    sub('runtime.c', '  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);view_step(r,now_us());\n',
        '  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);view_step(r,now_us());\n'
        '  /* v27: not in photo mode and no clip in flight: close Live\'s Wave5 stream the stock way */\n'
        '  if(!photo&&!LOAD(&r->stream.job)&&r->stream.encoder.hw){\n'
        '   int rel=live_encoder_release(&r->stream.encoder);\n'
        '   if(rel)crumb(r,now_us(),EV_W5,0xf0u|(uint32_t)(rel&0xf),0);}\n')
    print('v27 stock Wave5 close applied to', src)
