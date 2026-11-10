#!/usr/bin/env python3
"""v24 on top of v23: give the Wave5 instance and its image-pool memory back when Live is not in use.
Hardware 11-08: movie REC right after leaving photo mode hangs the camera even with Live switched off, but not
after a reboot with Live off: once Live has run, its Wave5 instance (16 MB arena + ~40 MB regions/slots from the
0x4c4155 image pool, 'geometry fixed per boot') stays pinned until reboot. Now the worker releases it (stream
EXIT if still open, both pool blocks freed) as soon as Live is off or the camera leaves photo mode and no Live
clip is being written; a later photo-mode frame allocates a fresh instance. A lock keeps this release and the
bring-up preempt hook (other task) apart. The v17 slot enlargement (1280x852 for the channel-2 experiment) is
reverted (slots sized for the actual frame again; channel 2 at 1280x852 would need its own instance).
Usage: v24_patch.py SRC_DIR (a copy of src-fix23)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    W = 'codec/live_w5enc.c'
    sub(W, '    uint64_t preempt_us; uint32_t reopen_blocked, pad22;   /* v22: no reopen within 3 s of a preempt */',
        '    uint64_t preempt_us; uint32_t reopen_blocked, owner_lock;   /* v22 cooldown; v24 preempt/release lock */')
    sub(W, '    if (client == W5_CLIENT || !w || w->magic != W5_MAGIC || !alive(w)) return;\n    __atomic_store_n(&w->release_req, 1, __ATOMIC_SEQ_CST);',
        '    if (client == W5_CLIENT || !w || w->magic != W5_MAGIC || !alive(w)) return;\n'
        '    uint32_t free_ = 0;\n'
        '    if (!__atomic_compare_exchange_n(&w->owner_lock, &free_, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;   /* v24: being released */\n'
        '    __atomic_store_n(&w->release_req, 1, __ATOMIC_SEQ_CST);')
    sub(W, '    w->frames = 0; w->batch_open = 0; w->preempts++; w->preempt_us = now_us();\n    __atomic_store_n(&w->release_req, 0, __ATOMIC_SEQ_CST);\n}',
        '    w->frames = 0; w->batch_open = 0; w->preempts++; w->preempt_us = now_us();\n    __atomic_store_n(&w->release_req, 0, __ATOMIC_SEQ_CST);\n'
        '    __atomic_store_n(&w->owner_lock, 0, __ATOMIC_RELEASE);\n}\n'
        '/* v24: worker task only, with no frame in flight: close the stream if open and free both pool blocks. */\n'
        'int live_encoder_release(live_encoder *e) {\n'
        '    struct live_w5 *w = e ? e->hw : 0;\n'
        '    if (!w) return 0;\n'
        '    if (w->magic != W5_MAGIC) { e->hw = 0; return 0; }\n'
        '    uint64_t start = now_us(); uint32_t free_ = 0;\n'
        '    while (!__atomic_compare_exchange_n(&w->owner_lock, &free_, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {\n'
        '        free_ = 0; if (now_us() - start > 2u * W5_PREEMPT_WAIT_US) return -1; DELAY(1);\n'
        '    }\n'
        '    if (alive(w)) EXIT(W5_CLIENT);\n'
        '    void *ad = w->arena_desc, *bd = w->block_desc;\n'
        '    w->magic = 0; e->hw = 0;\n'
        '    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n'
        '    if (ad) FN(0x4104d600, void (*)(void *))(ad);\n'
        '    if (bd) FN(0x4104d600, void (*)(void *))(bd);   /* the instance lives in this block */\n'
        '    return 1;\n'
        '}')
    sub(W, '    size_t sw = width > 1280u ? width : 1280u, sh = height > 852u ? height : 852u;\n',
        '    size_t sw = width, sh = height;   /* v24: v17 enlargement to 1280x852 reverted */\n')
    sub('codec/live_codec.h', 'void live_encoder_preempt(live_encoder *,uint32_t client);',
        'void live_encoder_preempt(live_encoder *,uint32_t client);\nint live_encoder_release(live_encoder *); /* v24 */')
    R = 'runtime.c'
    sub(R, '  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);view_step(r,now_us());\n',
        '  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);view_step(r,now_us());\n'
        '  /* v24: Live off or not in photo mode, nothing being written: give Wave5 and its pool memory back */\n'
        '  if((!photo||!LOAD(&r->requested))&&!LOAD(&r->stream.job)&&r->stream.encoder.hw){\n'
        '   int rel=live_encoder_release(&r->stream.encoder);crumb(r,now_us(),EV_W5,0xf0u|(uint32_t)(rel&0xf),(uint64_t)photo);}\n')
    print('v24 Wave5 release applied to', src)
