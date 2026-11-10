#!/usr/bin/env python3
"""v8 test aids (after picker_patch):
1. Live indicator re-presents every tune.icon_refresh_ms (default 500, 0 = off) even when the
   status is unchanged: switching monitor/EVF on half-press left the old overlay attached.
   UiState gains present/attach counters; icon failures are logged (EV_ICON).
2. Mux stage marks: JPG copy / MOV / LVP / MAN done -> EV_MUX_STAGE (a=stage, b=us since mux begin).
Usage: test_patch.py SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))
H = 'capture/live_stream.h'
sub(H, ' uint32_t picker;           /* live-view buffer choice, see native_capture.c */',
       ' uint32_t picker;           /* live-view buffer choice, see native_capture.c */\n uint32_t icon_refresh_ms;  /* re-present the Live indicator this often, 0 = only on change */')
sub(H, 'EV_FREEZE,EV_FREEZE_FAIL,EV_MUX_BEGIN,EV_MUX_END,EV_WRITER,EV_READY,EV_ENC_ERR,EV_MUX_ABORT,EV_NOTREADY};',
       'EV_FREEZE,EV_FREEZE_FAIL,EV_MUX_BEGIN,EV_MUX_END,EV_WRITER,EV_READY,EV_ENC_ERR,EV_MUX_ABORT,EV_NOTREADY,EV_ICON,EV_MUX_STAGE};')
sub('capture/live_stream.c', '1,3000000u,1,250000u,1,1,1,1,0};', '1,3000000u,1,250000u,1,1,1,1,0,500u};')
# indicator refresh
U = 'ui/live_ui.c'
sub(U, '#include <stddef.h>', '#include <stddef.h>\n#include "runtime.h"', 1)
sub(U, '    FxSceneCommand commands[16];\n} UiState;',
       '    FxSceneCommand commands[16];\n    uint64_t last_present_us;uint32_t presents,refreshes,present_fail,not_current;\n} UiState;')
sub(U, '''    if(u->display.attached&&!fx_native_display_is_current(&u->display)){hide(u);return;}
    if(status==u->display_status&&u->display.attached)return;''',
'''    if(u->display.attached&&!fx_native_display_is_current(&u->display)){u->not_current++;hide(u);return;}
    struct live_runtime *rt=atomic_load_explicit(&u->engine,memory_order_acquire);
    uint64_t t=0,f=0;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(t),"=r"(f));t=f?t/f*1000000u+(t%f)*1000000u/f:0;
    uint32_t every=rt?rt->stream.tune.icon_refresh_ms:0u;
    int refresh=every && t-u->last_present_us>=(uint64_t)every*1000u;
    if(status==u->display_status&&u->display.attached&&!refresh)return;
    if(refresh&&status==u->display_status)u->refreshes++;''')
sub(U, '''       fx_native_display_present(&u->display,&u->scene)==FX_OK)u->display_status=status;
    else hide(u);''', '''       fx_native_display_present(&u->display,&u->scene)==FX_OK){u->display_status=status;u->presents++;u->last_present_us=t;}
    else {u->present_fail++;u->last_present_us=t;if(rt)live_log(&rt->stream,t,EV_ICON,status,u->display.error);hide(u);}''')
# mux stages
T = 'codec/live_transaction.c'
sub(T, ' int r=f->close(f->opaque,in);in=-1;if(r)goto done;r=f->close(f->opaque,out);out=-1;if(r)goto done;\n }',
       ' int r=f->close(f->opaque,in);in=-1;if(r)goto done;r=f->close(f->opaque,out);out=-1;if(r)goto done;\n if(f->encode_event)f->encode_event(f->opaque,10);\n }')
sub(T, ' int r=f->close(f->opaque,video);video=-1;if(r)goto done;',
       ' int r=f->close(f->opaque,video);video=-1;if(r)goto done;\n if(f->encode_event)f->encode_event(f->opaque,11);')
sub(T, '  r=f->close(f->opaque,pv);pv=-1;if(r)goto done;',
       '  r=f->close(f->opaque,pv);pv=-1;if(r)goto done;\n  if(f->encode_event)f->encode_event(f->opaque,12);')
sub(T, ' m[z++]=\'\\n\';if(writeat(f,mf,0,m,z))goto done;r=f->close(f->opaque,mf);mf=-1;if(r)goto done;rc=0;',
       ' m[z++]=\'\\n\';if(writeat(f,mf,0,m,z))goto done;r=f->close(f->opaque,mf);mf=-1;if(r)goto done;rc=0;\n if(f->encode_event)f->encode_event(f->opaque,13);')
R = 'runtime.c'
sub(R, '''static void live_encode_event(void *p,unsigned event) {
 struct live_runtime *r=p;''', '''static void live_encode_event(void *p,unsigned event) {
 struct live_runtime *r=p;
 if(event>=10){live_log(&r->stream,now_us(),EV_MUX_STAGE,event-10,now_us()-r->encode_started_us);return;}''')
sub('capture/live_stream.c', 's->diag.version=8;', 's->diag.version=9;')
print('test patch applied to', src)
