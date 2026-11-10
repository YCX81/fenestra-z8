#!/usr/bin/env python3
"""v28 on top of v27: portrait Live playback with the camera held vertically, louder Live audio, audio crumbs.
1) Hardware 11-08: playback with the camera held vertically shows the portrait still right, but the Live frames
   squeezed with bars. The UI turns the 3:2 photo area with the camera; the v20 LVP of a portrait clip is the
   upright picture 452x682 pillarboxed in 1024x682, so it was fitted twice. Camera attitude: 0xa03bc8004
   (0 level, 1 grip up, 2 grip down; read on the camera in three poses). pbfix ui.c: when the camera is vertical
   and the frame is pillarboxed (black side bands), only the central 452x682 picture is handed to the scene and
   stretched over the area, which the rotated UI shows upright at its proportions. Level camera: unchanged.
2) Live audio was quiet (speech peaks about -16 dBFS, room -45 dBFS): x4 (+12 dB) with clipping, applied when the
   mux reads the samples.
3) Crumb EV_AUDIO (30) at each mux: a = audio state | audio_phase<<8 | audio_error<<16, b = audio_starts, to tell
   why a clip has no sound.
Usage: v28_patch.py SRC_DIR PB_DIR (copies of src-fix27 and pbfix/live-playback-ui)"""
import sys
from pathlib import Path
src = Path(sys.argv[1]) if __name__ == '__main__' else None
pb = Path(sys.argv[2]) if __name__ == '__main__' else None
def sub(root, path, old, new, count=1):
    p = root / path; t = p.read_text(); assert t.count(old) == count, (path, old[:70], t.count(old)); p.write_text(t.replace(old, new))
if __name__ == '__main__':
    U = 'ui.c'
    sub(pb, U, '    if(((uintptr_t)data&3u)||capacity<sizeof(FxSceneImage)+W*H*4u)return FX_BOUNDS;\n',
        '    if(((uintptr_t)data&3u)||capacity<sizeof(FxSceneImage)+W*H*4u)return FX_BOUNDS;\n'
        '    /* v28: camera held vertically (attitude 1/2) and a pillarboxed portrait frame: only the upright picture */\n'
        '    uint32_t X0=0u,SW=W;\n'
        '    if(W==1024u){\n'
        '        uint32_t att=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bc8004);\n'
        '        if(att==1u||att==2u){\n'
        '            const uint8_t *yp=i420;uint32_t dark=1u;\n'
        '            for(uint32_t r=8u;r<H&&dark;r+=48u)for(uint32_t x=0u;x<280u&&dark;x+=40u)\n'
        '                if(yp[r*W+x]>24u||yp[r*W+W-1u-x]>24u)dark=0u;\n'
        '            if(dark){SW=(H*H/W)&~3u;X0=((W-SW)/2u)&~1u;}\n'
        '        }\n'
        '    }\n')
    sub(pb, U, '    image.width=W;image.height=H;\n    image.stride=W*4u;', '    image.width=SW;image.height=H;\n    image.stride=SW*4u;')
    sub(pb, U, '        for(uint32_t col=0u;col<W;++col) {\n            uint32_t uv=(row/2u)*(W/2u)+col/2u;\n            int yy=(int)y[row*W+col]-16;',
        '        for(uint32_t oc=0u;oc<SW;++oc) {\n            uint32_t col=oc+X0;\n            uint32_t uv=(row/2u)*(W/2u)+col/2u;\n            int yy=(int)y[row*W+col]-16;')
    sub(pb, U, '            uint32_t pixel=(row*W+col)*4u;', '            uint32_t pixel=(row*SW+oc)*4u;')
    sub(pb, U, '    scene(s,c,3u,data,(uint32_t)(sizeof(FxSceneImage)+W*H*4u),capacity);return FX_OK;',
        '    scene(s,c,3u,data,(uint32_t)(sizeof(FxSceneImage)+SW*H*4u),capacity);return FX_OK;')
    R = 'runtime.c'
    sub(src, R, ' struct live_runtime *r=ctx;struct live_audio_info info;\n return live_audio_get(&r->audio,off,out,cap,got,&info);\n}',
        ' struct live_runtime *r=ctx;struct live_audio_info info;\n'
        ' int e=live_audio_get(&r->audio,off,out,cap,got,&info);\n'
        ' if(!e&&got)for(uint32_t i=0;i<*got;i++){int32_t s=(int32_t)out[i]*4;out[i]=(int16_t)(s>32767?32767:s<-32768?-32768:s);} /* v28: +12 dB */\n'
        ' return e;\n}')
    sub(src, R, ' if(audio==2){still.audio_get=live_audio_ticket_get;',
        ' crumb(r,now_us(),30u /* v28 EV_AUDIO */,(uint32_t)audio|(LOAD(&r->audio_phase)<<8)|((r->audio_error&0xffu)<<16),r->audio_starts);\n'
        ' if(audio==2){still.audio_get=live_audio_ticket_get;')
    print('v28 portrait vertical playback + audio gain applied to', src, pb)
