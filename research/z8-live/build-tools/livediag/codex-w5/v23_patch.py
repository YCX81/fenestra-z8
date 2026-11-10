#!/usr/bin/env python3
"""v23 on top of v22: record GUI view and playback-module changes as crumbs (USB Debug blocks playback, so the
host cannot watch it live). Each worker loop reads the native current view (0xa032280c8 -> view; id +0x54,
state +0xd0) and the playback runtime (registry +16: playing +76, frames +72, presented +8208, detached +8212,
render error +8216, left +8228, state-detached +8236); on a change it adds an entry to a separate RAM ring (vring[64], never saved to the card):
  EV_VIEW (28): a = id<<8 | state, b = view address
  EV_PB   (29): a = playing | presented<<8 (low 8 bits) | detached<<16 | left<<24, b = render error | state-detached<<32
Usage: v23_patch.py SRC_DIR (a copy of src-fix22)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    sub('capture/live_stream.h', 'EV_UI,EV_FLAG,EV_MODE,EV_W5,EV_LOGSAVE};', 'EV_UI,EV_FLAG,EV_MODE,EV_W5,EV_LOGSAVE,EV_VIEW,EV_PB};')
    sub('runtime.h', ' uint32_t ch2_req,ch2_state,ch2_applies,ch2_restores,ch2_reapplies,ch2_pad,ch2_saved[12]; /* v17 */\n};',
        ' uint32_t ch2_req,ch2_state,ch2_applies,ch2_restores,ch2_reapplies,ch2_pad,ch2_saved[12]; /* v17 */\n'
        ' uint64_t last_view,last_pb_b;uint32_t last_view_is,last_pb_a; /* v23 */\n'
        ' uint32_t vhead,vpad;struct live_ev vring[64]; /* v23: view/playback changes, RAM only (no card saves) */\n};')
    sub('runtime.c', 'static void mux_worker(void *arg) {',
        '/* v23: GUI view and playback-module changes as crumbs (all reads are RAM, range-checked). */\n'
        'static int v23_ram(uint64_t a,uint64_t n){return a>=UINT64_C(0xa00000000)&&a+n<=UINT64_C(0xa40000000)&&!(a&3);}\n'
        'static void vlog(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b){uint32_t i=r->vhead++%64u;r->vring[i]=(struct live_ev){t,type,a,b};}\n'
        'static void view_step(struct live_runtime *r,uint64_t now){\n'
        ' uint64_t v=*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa032280c8);uint32_t is=0xffffffffu;\n'
        ' if(v23_ram(v,0x110))is=(*(volatile uint32_t *)(uintptr_t)(v+0x54))<<8|((*(volatile uint32_t *)(uintptr_t)(v+0xd0))&0xffu);\n'
        ' if(v!=r->last_view||is!=r->last_view_is){r->last_view=v;r->last_view_is=is;vlog(r,now,EV_VIEW,is,v);}\n'
        ' uint64_t a0=*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa032b50d8);if(!v23_ram(a0,8))return;\n'
        ' uint64_t core=*(volatile uint64_t *)(uintptr_t)(a0+663*96+80);if(!v23_ram(core,2536))return;\n'
        ' uint64_t reg=*(volatile uint64_t *)(uintptr_t)(core+2528);if(!v23_ram(reg,24))return;\n'
        ' uint64_t pb=*(volatile uint64_t *)(uintptr_t)(reg+16);if(!v23_ram(pb,8240))return;\n'
        ' if(*(volatile uint32_t *)(uintptr_t)pb!=0x50425549u)return;\n'
        ' #define PB32(o) (*(volatile uint32_t *)(uintptr_t)(pb+(o)))\n'
        ' uint32_t a=(PB32(76)&0xffu)|(PB32(8208)&0xffu)<<8|(PB32(8212)&0xffu)<<16|(PB32(8228)&0xffu)<<24;\n'
        ' uint64_t b=(uint64_t)PB32(8216)|(uint64_t)PB32(8236)<<32;\n'
        ' if(a!=r->last_pb_a||b!=r->last_pb_b){r->last_pb_a=a;r->last_pb_b=b;vlog(r,now,EV_PB,a,b);}\n'
        ' #undef PB32\n'
        '}\n'
        'static void mux_worker(void *arg) {')
    sub('runtime.c', '  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);\n',
        '  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);view_step(r,now_us());\n')
    print('v23 view/playback crumbs applied to', src)
