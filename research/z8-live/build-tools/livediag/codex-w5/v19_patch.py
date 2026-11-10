#!/usr/bin/env python3
"""v19 on top of v18: channel-2 experiment, second try (static analysis of the stock scaler code, 11-08).
F_k +0x04 is a scaler mode (table 0x4164be10: 1/3/4), not a run bit; ch2 is already routed in photo LV
(A0 +0x08 = 0x07010000, outputs 0..2). The stock code only applies an output's new configuration when the
pipeline starts, or restarts one output with B_k +0x00 = 1, B_k +0x00 = 0, B_k +0x04 = 1 ('kick').
ch2_req (USB): 1 = test A: keep F2 +0x00/+0x04 (stock mode 4), set 1:1 1280x852 NV12, then kick B2;
               2 = variant: same plus F2 +0x00 = 0, +0x04 = 1 (ch0/ch1 mode), then kick;
               0 = restore the saved values, then kick.
Usage: v19_patch.py SRC_DIR (a copy of src-fix18)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
A = [(0x15423208, 0), (0x1542320c, 0), (0x15423210, 0x4000), (0x15423214, 0x4000), (0x1542322c, 1280), (0x15423230, 852),
     (0x1542220c, 0x01000000), (0x15422238, 1280), (0x1542223c, 1280), (0x15426720, 0x222)]
VARIANT = [(0x15423200, 0), (0x15423204, 1)]
SAVE = A + VARIANT
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    t = (src / 'runtime.c').read_text()
    a = t.index('static const uint32_t ch2_addr['); b = t.index('static void mux_worker(void *arg) {')
    na, ns = len(A), len(SAVE)
    sa = ','.join(f'0x{x:08x}u' for x, _ in SAVE); sv = ','.join(f'0x{v:x}u' for _, v in SAVE)
    new = (f'static const uint32_t ch2_addr[{ns}]={{{sa}}},ch2_val[{ns}]={{{sv}}};\n'
           '#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))\n'
           '/* v19: kick output 2 as the stock code restarts a single output: B2 +0x00 1 -> 0, then B2 +0x04 = 1 */\n'
           'static void ch2_kick(void){REG32(0x15422200u)=1u;__asm__ volatile("dsb sy":::"memory");REG32(0x15422200u)=0u;\n'
           ' __asm__ volatile("dsb sy":::"memory");REG32(0x15422204u)=1u;__asm__ volatile("dsb sy":::"memory");}\n'
           '/* v19: channel 2 at 1:1 on request (photo mode only); 1 = stock mode 4, 2 = mode 1 variant; 0 = restore. */\n'
           'static void ch2_step(struct live_runtime *r,int photo){\n'
           ' uint32_t q=LOAD(&r->ch2_req),s=r->ch2_state;\n'
           ' if((q==1u||q==2u)&&photo){\n'
           '  if(s==q&&REG32(0x1542322c)==1280u)return;\n'
           f'  if(s==1u||s==2u)r->ch2_reapplies++;else for(unsigned i=0;i<{ns}u;i++)r->ch2_saved[i]=REG32(ch2_addr[i]);\n'
           f'  unsigned n=q==2u?{ns}u:{na}u;for(unsigned i=0;i<n;i++)REG32(ch2_addr[i])=ch2_val[i];\n'
           '  __asm__ volatile("dsb sy":::"memory");ch2_kick();r->ch2_state=q;r->ch2_applies++;return;}\n'
           f' if(q==0u&&(s==1u||s==2u)){{for(unsigned i={ns}u;i-->0;)REG32(ch2_addr[i])=r->ch2_saved[i];\n'
           '  __asm__ volatile("dsb sy":::"memory");ch2_kick();r->ch2_state=0u;r->ch2_restores++;}\n'
           '}\n')
    (src / 'runtime.c').write_text(t[:a] + new + t[b:])
    assert ns <= 12
    print('v19 channel-2 kick experiment applied to', src)
