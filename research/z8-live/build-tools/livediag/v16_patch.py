#!/usr/bin/env python3
"""v16 on top of v15: read-only register record of the live-view scaler, to find what keeps output channel 2
from writing in photo mode (hardware 11-07: ch2 configured 1920x1080 NV16 in image-bank RAM, never written).
Once a second the worker copies, with 32-bit loads, the words listed in LV_REGS (below) into
struct live_runtime.lv_regs[]. Only offsets the firmware itself programs are read (the QEMU model's
engine1542_mask for registration 40c2590c); status words (+0x1c of 0x15421000/0x15422x00) and unprogrammed
offsets are never touched - an unmodelled read stopped QEMU strict mode, and on hardware could fault.
Usage: v16_patch.py SRC_DIR (a copy of src-fix15)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
IN = [0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28, 0x2c, 0x38, 0x3c, 0x40, 0x48, 0x4c, 0x50, 0x54]
B = [0x00, 0x0c, 0x10, 0x14, 0x18, 0x20, 0x24, 0x28, 0x2c, 0x38, 0x3c, 0x40, 0x48, 0x60, 0x6c, 0x70]
F = [0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38]
P = [0x700, 0x704, 0x708, 0x70c, 0x710, 0x714, 0x720, 0x724, 0x730, 0x734, 0x738, 0x73c, 0x740, 0x744, 0x748, 0x74c, 0x780]
LV_REGS = ([0x15421000 + o for o in IN] + [0x15422000 + k * 0x100 + o for k in range(3) for o in B] +
           [0x15423000 + k * 0x100 + o for k in range(3) for o in F] + [0x15425000 + k * 0x800 + o for k in range(3) for o in P])
assert len(LV_REGS) <= 228
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text(); assert t.count(old) == count, (path, old[:60], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    sub('runtime.h', ' uint64_t lv_snap[4],lv_geo_t; /* v14: last snapshot Y address per output, time of lv_geo */\n};',
        ' uint64_t lv_snap[4],lv_geo_t; /* v14: last snapshot Y address per output, time of lv_geo */\n'
        ' uint32_t lv_regs[228],lv_regs_n; /* v16: scaler register record, order = v16_patch.LV_REGS */\n'
        ' uint32_t lv_regs_on,lv_regs_pad; /* v16: 1 = record (set over USB); default 0, nothing is read */\n};')
    table = ','.join(f'0x{a:08x}u' for a in LV_REGS)
    sub('runtime.c', 'static void mux_worker(void *arg) {',
        f'static const uint32_t lv_reg_addr[{len(LV_REGS)}]={{{table}}};\nstatic void mux_worker(void *arg) {{')
    sub('runtime.c', '    r->lv_geo[k][0]=g.width;r->lv_geo[k][1]=g.height;r->lv_geo[k][2]=g.y_stride;r->lv_geo[k][3]=g.uv_stride;r->lv_geo[k][4]=g.nv12;r->lv_geo[k][5]=ok;}}\n',
        '    r->lv_geo[k][0]=g.width;r->lv_geo[k][1]=g.height;r->lv_geo[k][2]=g.y_stride;r->lv_geo[k][3]=g.uv_stride;r->lv_geo[k][4]=g.nv12;r->lv_geo[k][5]=ok;}\n'
        f'   if(r->lv_regs_on==1u){{for(unsigned i=0;i<{len(LV_REGS)}u;i++)r->lv_regs[i]=*(volatile uint32_t *)(uintptr_t)lv_reg_addr[i];\n'
        '   r->lv_regs_n++;}}\n')
    print('v16 register record applied to', src, len(LV_REGS), 'words')
