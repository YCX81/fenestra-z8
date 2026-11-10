#!/usr/bin/env python3
"""v17 on top of v16: experimental 1:1 live-view output on scaler channel 2 (1280x852 NV12), on demand.
Hardware 11-07/08 (v16 register record): the scaler input is 1280x852; channel 2 is configured 1920x1080 NV16
for HDMI and never written in photo or movie live view (F2 +0x04 = 4, F0/F1 +0x04 = 1); in movie mode the
firmware itself runs a 1:1 output with scale 0x4000 and F +0x00 = 0. On USB command (ch2_req = 1) the worker
saves and rewrites CH2_REGS (run bit F2 +0x04 last), and restores them in reverse order on ch2_req = 0.
If the firmware reprograms channel 2 while ch2_req = 1 (photo mode), the worker re-applies it.
Live side: the image banks (0x840000000..0x8fff47400) are accepted as frame addresses; the picker forgets
the learned buffers when the channel changes; the Wave5 instance reserves slots for 1280x852 and reopens
its stream when the size changes (it was fixed per boot).
Usage: v17_patch.py SRC_DIR (a copy of src-fix16)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
CH2_REGS = [(0x15423200, 0), (0x15423208, 0), (0x1542320c, 0), (0x15423210, 0x4000), (0x15423214, 0x4000),
            (0x1542322c, 1280), (0x15423230, 852), (0x1542220c, 0x01000000), (0x15422238, 1280), (0x1542223c, 1280),
            (0x15426720, 0x222), (0x15423204, 1)]
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    n = len(CH2_REGS)
    sub('runtime.h', ' uint32_t lv_regs_on,lv_regs_pad; /* v16: 1 = record (set over USB); default 0, nothing is read */\n};',
        ' uint32_t lv_regs_on,lv_regs_pad; /* v16: 1 = record (set over USB); default 0, nothing is read */\n'
        f' uint32_t ch2_req,ch2_state,ch2_applies,ch2_restores,ch2_reapplies,ch2_pad,ch2_saved[{n}]; /* v17 */\n}};')
    addr = ','.join(f'0x{a:08x}u' for a, _ in CH2_REGS); val = ','.join(f'0x{v:x}u' for _, v in CH2_REGS)
    sub('runtime.c', 'static void mux_worker(void *arg) {',
        f'static const uint32_t ch2_addr[{n}]={{{addr}}},ch2_val[{n}]={{{val}}};\n'
        '#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))\n'
        '/* v17: channel 2 at 1:1 on request (photo mode only); run bit written last, restored first. */\n'
        'static void ch2_step(struct live_runtime *r,int photo){\n'
        ' uint32_t q=LOAD(&r->ch2_req),s=r->ch2_state;\n'
        f' if(q==1u&&photo){{\n'
        f'  if(s==1u&&REG32(0x15423204)==1u&&REG32(0x1542322c)==1280u)return;\n'
        f'  if(s==1u)r->ch2_reapplies++;else for(unsigned i=0;i<{n}u;i++)r->ch2_saved[i]=REG32(ch2_addr[i]);\n'
        f'  for(unsigned i=0;i<{n}u;i++)REG32(ch2_addr[i])=ch2_val[i];\n'
        '  __asm__ volatile("dsb sy":::"memory");r->ch2_state=1u;r->ch2_applies++;return;}\n'
        f' if(q==0u&&s==1u){{for(unsigned i={n}u;i-->0;)REG32(ch2_addr[i])=r->ch2_saved[i];\n'
        '  __asm__ volatile("dsb sy":::"memory");r->ch2_state=0u;r->ch2_restores++;}\n'
        '}\nstatic void mux_worker(void *arg) {')
    sub('runtime.c', '  flag_step(r,now_us());log_step(r,now_us(),0);\n', '  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);\n')
    N = 'capture/native_capture.c'
    sub(N, '     for(unsigned k=0;k<4;k++)r->lv_snap[k]=s[3+k];\n',
        '     for(unsigned k=0;k<4;k++)r->lv_snap[k]=s[3+k];\n'
        '     if(ch!=r->lv_pad){st->diag.nbufs=0;st->trans_n=0;st->last_y=0;st->last_kept_y=0;r->lv_pad=ch;} /* v17: new channel, new buffers */\n')
    sub(N, '    return *y>=UINT64_C(0xa00000000) && *y<UINT64_C(0xa40000000) &&\n           *uv>=UINT64_C(0xa00000000) && *uv<UINT64_C(0xa40000000);',
        '    /* v17: work RAM, or the image banks (channel 2 buffers) */\n'
        '    #define LA_RAM(a) (((a)>=UINT64_C(0xa00000000)&&(a)<UINT64_C(0xa40000000))||((a)>=UINT64_C(0x840000000)&&(a)<UINT64_C(0x8fff47400)))\n'
        '    return LA_RAM(*y) && LA_RAM(*uv);')
    W = 'codec/live_w5enc.c'
    sub(W, '        if (w->magic != W5_MAGIC || w->width != width || w->height != height) return -1;',
        '        if (w->magic != W5_MAGIC) return -1;\n'
        '        if (w->width != width || w->height != height) {\n'
        '            /* v17: another live-view channel: reopen the stream at the new size if the slots fit it */\n'
        '            if ((size_t)width * ((height + 31) & ~31u) * 3 / 2 > w->slot_bytes) return -1;\n'
        '            if (alive(w)) EXIT(W5_CLIENT);\n'
        '            w->width = width; w->height = height;\n'
        '        }')
    sub(W, '    uint32_t slot = (uint32_t)(((size_t)width * ((height + 31) & ~31u) * 3 / 2 + 0xfff) & ~(size_t)0xfff);',
        '    /* v17: slots for the larger of this size and 1280x852 (channel 2 at 1:1), so the size may change later */\n'
        '    size_t sw = width > 1280u ? width : 1280u, sh = height > 852u ? height : 852u;\n'
        '    uint32_t slot = (uint32_t)((sw * ((sh + 31) & ~(size_t)31) * 3 / 2 + 0xfff) & ~(size_t)0xfff);')
    print('v17 channel-2 patch applied to', src)
