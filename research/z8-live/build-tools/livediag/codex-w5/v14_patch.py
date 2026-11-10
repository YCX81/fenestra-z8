#!/usr/bin/env python3
"""v14 Live changes on top of v13.1 (src-fix13): selectable live-view output channel.
Live copied output channel 1 of the live-view scaler (1024x682 in photo mode) - the picture quality limit
the user saw on hardware (11-07). The scaler has four outputs: geometry F_k = 0x15423000 + k*0x100
(+0x2c width, +0x30 height), stride/format B_k = 0x15422000 + k*0x100 (+0x38/+0x3c strides, +0x0c/+0x10
format), and the snapshot 0x40c269cc(0x15431000) gives Y = s[3+k], UV = s[10+k].
  - struct live_runtime gains lv_channel (appended; default 1 = v13.1 behaviour), switchable over USB.
  - la_native_geometry and the snapshot picker use that channel. A channel with no valid NV12 geometry
    simply fails the lease (Live keeps waiting); a size change goes through the existing geometry path
    (frames skipped, Live restarts with buffers for the new size after geometry_reset_us).
Usage: v14_patch.py SRC_DIR (a copy of src-fix13 with v13_patch applied)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])


def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))


RH = 'runtime.h'
sub(RH, ' uint32_t crumb_head,crumb_pad;struct live_ev crumbs[32];\n};',
    ' uint32_t crumb_head,crumb_pad;struct live_ev crumbs[32];\n'
    ' uint32_t lv_channel,lv_pad; /* v14: live-view scaler output Live copies (0..3), default 1 */\n};')
R = 'runtime.c'
sub(R, ' r->magic=LIVE_RUNTIME_MAGIC;r->size=sizeof(*r);\n', ' r->magic=LIVE_RUNTIME_MAGIC;r->size=sizeof(*r);r->lv_channel=1;\n')
N = 'capture/native_capture.c'
sub(N, 'int la_native_geometry(struct la_native_geometry *g) {\n    if(!g)return 0;',
    'int la_native_geometry(struct la_native_geometry *g) {return la_native_geometry_k(g,1);}\n'
    'int la_native_geometry_k(struct la_native_geometry *g,unsigned k) {\n    if(!g||k>3)return 0;uintptr_t o=(uintptr_t)k*0x100u;')
sub(N, '    g->width=read32(0x1542312c);g->height=read32(0x15423130);\n    g->y_stride=read32(0x15422138);g->uv_stride=read32(0x1542213c);\n    g->nv12=!(read32(0x1542210c)&0x10) && read32(0x15422110)==1;',
    '    g->width=read32(0x1542302c+o);g->height=read32(0x15423030+o);\n    g->y_stride=read32(0x15422038+o);g->uv_stride=read32(0x1542203c+o);\n    g->nv12=!(read32(0x1542200c+o)&0x10) && read32(0x15422010+o)==1;')
NH = 'capture/native_capture.h'
sub(NH, 'int la_native_geometry(struct la_native_geometry *);', 'int la_native_geometry(struct la_native_geometry *);\nint la_native_geometry_k(struct la_native_geometry *,unsigned k);')
sub(N, '    struct live_stream *st=&r->stream;uint32_t mode=st->tune.picker;\n',
    '    struct live_stream *st=&r->stream;uint32_t mode=st->tune.picker;unsigned ch=r->lv_channel<=3?r->lv_channel:1u;\n')
sub(N, '     uintptr_t cur=norm(s[4]);', '     uintptr_t cur=norm(s[3+ch]);')
sub(N, '    else{*y=s[4];*uv=s[11];} /* C0+a0/a8 */', '    else{*y=s[3+ch];*uv=s[10+ch];} /* C0+a0/a8 (channel 1); v14: channel ch */')
sub(N, '    struct la_capture *c=ctx;struct la_native_geometry g;uintptr_t y,uv;\n    uint32_t epoch=__atomic_load_n(&c->cancel_epoch,__ATOMIC_ACQUIRE);\n    if(!la_native_geometry(&g)||',
    '    struct la_capture *c=ctx;struct la_native_geometry g;uintptr_t y,uv;\n    uint32_t epoch=__atomic_load_n(&c->cancel_epoch,__ATOMIC_ACQUIRE);\n'
    '    struct live_runtime *rt=(struct live_runtime *)((uint8_t *)c-offsetof(struct live_runtime,capture));\n'
    '    if(!la_native_geometry_k(&g,rt->lv_channel<=3?rt->lv_channel:1u)||')
print('v14 channel patch applied to', src)

# 2. Diagnostics for choosing the channel: the host must not read the 0x1542xxxx registers itself (the USB
#    debug read is not a 32-bit access and hung the camera twice on 11-07). The worker records the geometry
#    of all four outputs once a second with the same 32-bit loads Live uses, and the picker records the
#    snapshot Y addresses of all four, so the host reads only ordinary RAM.
sub(RH, ' uint32_t lv_channel,lv_pad; /* v14: live-view scaler output Live copies (0..3), default 1 */\n};',
    ' uint32_t lv_channel,lv_pad; /* v14: live-view scaler output Live copies (0..3), default 1 */\n'
    ' uint32_t lv_geo[4][6]; /* v14: per output: width, height, y stride, uv stride, nv12, valid */\n'
    ' uint64_t lv_snap[4],lv_geo_t; /* v14: last snapshot Y address per output, time of lv_geo */\n};')
sub(N, '     uintptr_t cur=norm(s[3+ch]);', '     for(unsigned k=0;k<4;k++)r->lv_snap[k]=s[3+k];\n     uintptr_t cur=norm(s[3+ch]);')
sub(R, '  flag_step(r,now_us());log_step(r,now_us(),0);\n',
    '  flag_step(r,now_us());log_step(r,now_us(),0);\n'
    '  if(now_us()-r->lv_geo_t>1000000u){r->lv_geo_t=now_us();\n'
    '   for(unsigned k=0;k<4;k++){struct la_native_geometry g={0};uint32_t ok=(uint32_t)la_native_geometry_k(&g,k);\n'
    '    r->lv_geo[k][0]=g.width;r->lv_geo[k][1]=g.height;r->lv_geo[k][2]=g.y_stride;r->lv_geo[k][3]=g.uv_stride;r->lv_geo[k][4]=g.nv12;r->lv_geo[k][5]=ok;}}\n')
print('v14 channel diagnostics applied')
