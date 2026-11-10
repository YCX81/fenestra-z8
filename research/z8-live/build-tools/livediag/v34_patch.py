#!/usr/bin/env python3
"""v34 on top of v33: card lamp as an independent marker for the Wave5 hand-back; the flow is otherwise unchanged.
GPIO 139 (stock output 0x40c15c3c, descriptor 0xa0280a1b0, level 1 = lit, confirmed on the camera 11-09): lit at entry to
the bring-up notify for any client other than Live, held at least 800 ms, off when the notify returns.
Usage: v34_patch.py SRC_DIR (a copy of src-fix33)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]); p = src / 'runtime.c'; t = p.read_text()
old = "if(r){crumb(r,now_us(),EV_W5,client,LOAD(&r->stream.job));live_encoder_preempt(&r->stream.encoder,client);}"
assert t.count(old) == 1
new = ("{uint64_t lamp_t=now_us();int lamp=client!=4u;\n"
       "  if(lamp)((void(*)(void*,uint32_t))(uintptr_t)UINT64_C(0x40c15c3c))((void*)(uintptr_t)UINT64_C(0xa0280a1b0),1u); /* v34 */\n"
       "  " + old + "\n"
       "  if(lamp){while(now_us()-lamp_t<800000u)((int32_t(*)(uint32_t))(uintptr_t)UINT64_C(0x40187b00))(1u);\n"
       "   ((void(*)(void*,uint32_t))(uintptr_t)UINT64_C(0x40c15c3c))((void*)(uintptr_t)UINT64_C(0xa0280a1b0),0u);}}")
p.write_text(t.replace(old, new)); print('v34 lamp marker applied to', src)
