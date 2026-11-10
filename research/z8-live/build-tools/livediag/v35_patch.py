#!/usr/bin/env python3
"""v35 on top of v34: card lamp code of the Wave5 state left by the hand-back (diagnostic only; the close flow is unchanged).
After the hook's close and the 800 ms hold: off 400 ms, group 1, off 400 ms, group 2, off 400 ms, group 3, each flash 150 ms on / 150 ms off.
Group 1 (state): 1 fully closed (VDI 0, service down, both workers 0, both slots free, cores free, client 4 unmapped),
2 service still up but slot/core free, 3 slot full or core A busy, 4 anything else, 5 no Live encoder.
Group 2: (fin_rc stage) + 1. Group 3: 1 last close succeeded, 2 otherwise (not attempted or failed).
Once per boot for a real Live encoder. Blinking adds about 2-6 s to the REC start.
Usage: v35_patch.py SRC_DIR (a copy of src-fix34)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(f, old, new):
    p = src / f; t = p.read_text(); assert t.count(old) == 1, (old[:70], t.count(old)); p.write_text(t.replace(old, new))
sub('codec/live_w5enc.c', 'void live_encoder_preempt(live_encoder *e, uint32_t client) {', r'''/* v35: lamp code of the state the hand-back left; returns 0 when already shown this boot */
int live_encoder_diag_code(live_encoder *e, uint32_t out[3]) {
    struct live_w5 *w = e ? e->hw : 0;
    if (!w || w->magic != W5_MAGIC) { out[0] = 5; out[1] = 1; out[2] = 2; return 1; }
    if (w->pad33) return 0;
    w->pad33 = 1;
    uint32_t up = RD32(0xa02d30110), w0 = WSTATE(0), w1 = WSTATE(1), s0 = SLOT_USED(0), s1 = SLOT_USED(1), a = RD32(0xa02d30130), b = RD32(0xa02d30134);
    if (!VDI_BASE && !up && !w0 && !w1 && !s0 && !s1 && !a && !b && CLIENT_STREAM(W5_CLIENT) == 0xff) out[0] = 1;
    else if ((s0 && s1) || a) out[0] = 3;
    else if (up) out[0] = 2;
    else out[0] = 4;
    out[1] = (w->fin_rc >> 24) + 1u;
    out[2] = (w->closes && !(w->fin_rc >> 24) && w->eos_ok) ? 1u : 2u;
    return 1;
}
void live_encoder_preempt(live_encoder *e, uint32_t client) {''')
sub('codec/live_codec.h', 'void live_encoder_diag_tick(live_encoder *); /* v33 */', 'void live_encoder_diag_tick(live_encoder *); /* v33 */\nint live_encoder_diag_code(live_encoder *,uint32_t out[3]); /* v35 */')
sub('runtime.c', '/* Stock Wave5 bring-up hook (0x402f2d58), caller task: give Wave5 back to movie/playback. */',
r'''/* v35: card lamp (GPIO 139, level 1 = lit) */
static void lamp_set(uint32_t on){((void(*)(void*,uint32_t))(uintptr_t)UINT64_C(0x40c15c3c))((void*)(uintptr_t)UINT64_C(0xa0280a1b0),on);}
static void lamp_wait(uint32_t us){uint64_t t=now_us();while(now_us()-t<us)((int32_t(*)(uint32_t))(uintptr_t)UINT64_C(0x40187b00))(1u);}
static void lamp_code(const uint32_t c[3]){for(unsigned g=0;g<3;g++){lamp_set(0);lamp_wait(400000u);
 for(uint32_t i=0;i<c[g]&&i<16u;i++){lamp_set(1);lamp_wait(150000u);lamp_set(0);lamp_wait(150000u);}}}
/* Stock Wave5 bring-up hook (0x402f2d58), caller task: give Wave5 back to movie/playback. */''')
sub('runtime.c', '   ((void(*)(void*,uint32_t))(uintptr_t)UINT64_C(0x40c15c3c))((void*)(uintptr_t)UINT64_C(0xa0280a1b0),0u);}}',
    '   lamp_set(0);\n   {uint32_t code[3];if(r?live_encoder_diag_code(&r->stream.encoder,code):(code[0]=5u,code[1]=1u,code[2]=2u,1))lamp_code(code);}}}')
print('v35 lamp code applied to', src)
