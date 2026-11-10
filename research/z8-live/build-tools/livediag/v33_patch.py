#!/usr/bin/env python3
"""v33 on top of v32: diagnostic only. The hand-back flow is unchanged; each step is recorded to A:\\LIVEDATA\\LIVEW5.BIN.
The hook records to RAM only; Live's worker writes A/B files LIVEW5A/B.BIN after new records (rate of the worker loop). 32 records of 64 bytes:
u32 seq, stage; u64 t_us; u32 a, b, worker, inst_used, inst_state, vdi_lo, svc_up, map4, slot_used, core_a, busy, pad.
Stages: 1 hook enter (a=client, b=alive), 2 busy wait over, 10 before STOP, 11 before normal TRIGGER, 12 picture done,
13 before drain, 14 worker 5, 15 before FINISH, 16 after FINISH, 17 before EXIT, 18 EXIT returned, 19 VDI down,
30 close returned (a=fin_rc), 40 worker tick after the hook (12 ticks, 250 ms apart).
Usage: v33_patch.py SRC_DIR (a copy of src-fix32)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
W = 'codec/live_w5enc.c'
def sub(old, new, f=W, count=1):
    p = src / f; t = p.read_text(); assert t.count(old) == count, (old[:70], t.count(old)); p.write_text(t.replace(old, new))
sub('    uint32_t close_lock, close_failed;   /* v30 */',
    '    uint32_t close_lock, close_failed;   /* v30 */\n    uint32_t diag_n, diag_ticks, diag_saved, diag_ab, diag_err, pad33; uint64_t diag_last, diag_retry;   /* v33 */\n    uint8_t diag[2048] __attribute__((aligned(16))), diag_snap[2048] __attribute__((aligned(16)));   /* v33 */')
HELP = r'''/* v33: the hook only appends to RAM; the worker task puts the records on the card (never file calls in the hook) */
static void w5_diag(struct live_w5 *w, uint32_t stage, uint32_t a, uint32_t b) {
    uint32_t i = __atomic_load_n(&w->diag_n, __ATOMIC_ACQUIRE);
    do { if (i >= 32u) return; } while (!__atomic_compare_exchange_n(&w->diag_n, &i, i + 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    uint32_t s = w->stream <= 1u ? w->stream : 0u;
    volatile uint32_t *inst = (volatile uint32_t *)(uintptr_t)UINT64_C(0xa02d41848);   /* static instance slot 0 */
    uint32_t r[16] = {0, stage, 0, 0, a, b, WSTATE(s), inst[0], inst[3],
                      (uint32_t)VDI_BASE, RD32(0xa02d30110), CLIENT_STREAM(W5_CLIENT), SLOT_USED(s), RD32(0xa02d30130), w->busy, 0};
    uint64_t t = now_us(); memcpy(&r[2], &t, 8);
    memcpy(w->diag + i * 64u + 4u, &r[1], 60);
    __atomic_store_n((uint32_t *)(void *)(w->diag + i * 64u), i + 1u, __ATOMIC_RELEASE);   /* record valid */
}
/* v33: worker task: what happened next, and flush new records (A/B files, so a torn write keeps the other) */
void live_encoder_diag_tick(live_encoder *e) {
    struct live_w5 *w = e ? e->hw : 0;
    if (!w || w->magic != W5_MAGIC) return;
    uint64_t t = now_us();
    if (w->preempts && w->diag_ticks < 12u && t - w->diag_last >= 250000u) { w->diag_last = t; w->diag_ticks++; w5_diag(w, 40u, w->diag_ticks, RD32(0xa03bd062c)); }
    /* only complete records, copied once; saved count moves only after a good write; a failed write retries at 200 ms */
    uint32_t n = 0;
    while (n < 32u && __atomic_load_n((uint32_t *)(void *)(w->diag + n * 64u), __ATOMIC_ACQUIRE) == n + 1u) n++;
    if (n == w->diag_saved || (w->diag_retry && t - w->diag_retry < 200000u)) return;
    memset(w->diag_snap, 0, 2048u); memcpy(w->diag_snap, w->diag, n * 64u);
    w->diag_ab ^= 1u;
    int32_t fd = FN(0x402dd4f8, int32_t (*)(const char *, uint32_t, uint32_t))(w->diag_ab ? "A:\\LIVEDATA\\LIVEW5B.BIN" : "A:\\LIVEDATA\\LIVEW5A.BIN", 0x301u, 0u);
    if (fd < 0) { w->diag_err++; w->diag_retry = t; return; }
    uint32_t written = 0;
    uint32_t e1 = FN(0x402dd78c, uint32_t (*)(int32_t, const void *, uint32_t, uint32_t *, void *))(fd, w->diag_snap, 2048u, &written, 0);
    uint32_t e2 = FN(0x402dd558, uint32_t (*)(int32_t))(fd);
    if (e1 || e2 || written != 2048u) { w->diag_err++; w->diag_retry = t; return; }
    w->diag_saved = n; w->diag_retry = 0;
}
'''
sub('static int w5_close_body(struct live_w5 *w) {', HELP + 'static int w5_close_body(struct live_w5 *w) {')
sub('    if (W5_STOP(s)) return 3;', '    w5_diag(w, 10u, k, w->frames);\n    if (W5_STOP(s)) return 3;')
sub('    if (TRIGGER(s, 0, k, 0, 0)) return 4;', '    w5_diag(w, 11u, k, 0);\n    if (TRIGGER(s, 0, k, 0, 0)) return 4;')
sub('    w->hw_index++; w->frames++;\n    if (TRIGGER(s, 0, (k + 1) % W5_SLOTS, 0, 0)) return 6;   /* drain */',
    '    w->hw_index++; w->frames++;\n    w5_diag(w, 12u, 0, 0);\n    w5_diag(w, 13u, 0, 0);\n    if (TRIGGER(s, 0, (k + 1) % W5_SLOTS, 0, 0)) return 6;   /* drain */')
sub('    w->fin_rc = 5u << 8;\n    W5_FINISH(s);\n', '    w5_diag(w, 14u, 0, 0);\n    w->fin_rc = 5u << 8;\n    w5_diag(w, 15u, 0, 0);\n    W5_FINISH(s);\n    w5_diag(w, 16u, 0, 0);\n')
sub('    EXIT(W5_CLIENT);\n    t = now_us();\n    while (VDI_BASE) { if (now_us() - t > 1000000u) return 9; DELAY(1); }\n    return 0;',
    '    w5_diag(w, 17u, 0, 0);\n    EXIT(W5_CLIENT);\n    w5_diag(w, 18u, 0, 0);\n    t = now_us();\n    while (VDI_BASE) { if (now_us() - t > 1000000u) return 9; DELAY(1); }\n    w5_diag(w, 19u, 0, 0);\n    return 0;')
sub('    if (client == W5_CLIENT || !w || w->magic != W5_MAGIC || !alive(w)) return;\n    __atomic_store_n(&w->release_req, 1, __ATOMIC_SEQ_CST);\n    uint64_t start = now_us();',
    '    if (client == W5_CLIENT || !w || w->magic != W5_MAGIC) return;\n    w5_diag(w, 1u, client, (uint32_t)alive(w));\n    if (!alive(w)) return;\n    __atomic_store_n(&w->release_req, 1, __ATOMIC_SEQ_CST);\n    uint64_t start = now_us();')
sub('    else if (alive(w)) w5_close(w);   /* v27: the worker may have released it meanwhile */\n',
    '    else { w5_diag(w, 2u, w->busy, 0); if (alive(w)) w5_close(w); }   /* v27: the worker may have released it meanwhile */\n    w5_diag(w, 30u, w->fin_rc, w->close_failed);\n')
sub('void live_encoder_preempt(live_encoder *,uint32_t client);', 'void live_encoder_preempt(live_encoder *,uint32_t client);\nvoid live_encoder_diag_tick(live_encoder *); /* v33 */', f='codec/live_codec.h')
sub('  /* v32: no close at the mode switch; the bring-up hook hands Wave5 back */\n',
    '  /* v32: no close at the mode switch; the bring-up hook hands Wave5 back */\n  if(r->stream.encoder.hw)live_encoder_diag_tick(&r->stream.encoder); /* v33 */\n', f='runtime.c')
print('v33 diagnostics applied to', src)
