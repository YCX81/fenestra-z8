#!/usr/bin/env python3
"""Playback layer: 1024x682 Live previews (same size/ratio as the Live clip; old 320x212 still play),
buffers allocated separately (native heap limit ~4 MB per block), and a full-canvas image so the
1280x800 scene maps 1:1 onto the 3:2 photo area (the 1207-unit image was ~6% too narrow).
Usage: pb_hires_patch.py PB_SRC_DIR"""
import sys
from pathlib import Path
src = Path(sys.argv[1])
def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))
sub('ui.h', '#define PB_PREVIEW_WIDTH 320u', '#define PB_PREVIEW_WIDTH 1024u   /* largest accepted preview (Live clip size); 320x212 also plays */')
sub('ui.h', '#define PB_PREVIEW_HEIGHT 212u', '#define PB_PREVIEW_HEIGHT 682u')
sub('ui.c', '''    if(((uintptr_t)data&3u)||bytes!=PB_PREVIEW_FRAME_BYTES||capacity<PB_SCENE_DATA_BYTES)return FX_BOUNDS;''',
'''    uint32_t W,H;
    if(bytes==320u*212u*3u/2u){W=320u;H=212u;}else if(bytes==1024u*682u*3u/2u){W=1024u;H=682u;}else return FX_BOUNDS;
    if(((uintptr_t)data&3u)||capacity<sizeof(FxSceneImage)+W*H*4u)return FX_BOUNDS;''')
sub('ui.c', '''    image.width=PB_PREVIEW_WIDTH;image.height=PB_PREVIEW_HEIGHT;
    image.stride=PB_PREVIEW_WIDTH*4u;image.format=FX_SCENE_RGBA8;''', '''    image.width=W;image.height=H;
    image.stride=W*4u;image.format=FX_SCENE_RGBA8;''')
sub('ui.c', '''    const uint8_t *y=i420,*u=y+PB_PREVIEW_WIDTH*PB_PREVIEW_HEIGHT;
    const uint8_t *v=u+PB_PREVIEW_WIDTH*PB_PREVIEW_HEIGHT/4u;''', '''    const uint8_t *y=i420,*u=y+W*H;
    const uint8_t *v=u+W*H/4u;''')
sub('ui.c', '''    for(uint32_t row=0u;row<PB_PREVIEW_HEIGHT;++row) {
        for(uint32_t col=0u;col<PB_PREVIEW_WIDTH;++col) {
            uint32_t uv=(row/2u)*(PB_PREVIEW_WIDTH/2u)+col/2u;
            int yy=(int)y[row*PB_PREVIEW_WIDTH+col]-16;''', '''    for(uint32_t row=0u;row<H;++row) {
        for(uint32_t col=0u;col<W;++col) {
            uint32_t uv=(row/2u)*(W/2u)+col/2u;
            int yy=(int)y[row*W+col]-16;''')
sub('ui.c', '            uint32_t pixel=(row*PB_PREVIEW_WIDTH+col)*4u;', '            uint32_t pixel=(row*W+col)*4u;')
sub('ui.c', '''    c[2]=command(FX_SCENE_IMAGE,36u,0u,1207u,800u,0u);
    scene(s,c,3u,data,PB_SCENE_DATA_BYTES,capacity);return FX_OK;''', '''    c[2]=command(FX_SCENE_IMAGE,0u,0u,1280u,800u,0u);   /* the scene already spans the 3:2 photo area */
    scene(s,c,3u,data,(uint32_t)(sizeof(FxSceneImage)+W*H*4u),capacity);return FX_OK;''')
sub('runtime.h', '    uint8_t i420[101760],pixels[271384];', '    uint8_t *i420,*pixels;   /* PB_PREVIEW_FRAME_BYTES / PB_SCENE_DATA_BYTES, separate heap blocks */')
sub('runtime.c', '''lp_preview_open(h,n,&r->preview,LOAD(&r->key_epoch))||r->preview.width!=320||r->preview.height!=212){hide(r);return 0;}''',
'''lp_preview_open(h,n,&r->preview,LOAD(&r->key_epoch))||
    !((r->preview.width==320&&r->preview.height==212)||(r->preview.width==1024&&r->preview.height==682))){hide(r);return 0;}''')
sub('runtime.c', ' uint32_t index=(uint32_t)((off-32)/101760);if(index==r->last_frame)return;',
       ' uint32_t index=(uint32_t)((off-32)/r->preview.frame_bytes);if(index==r->last_frame)return;')
sub('runtime.c', 'FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(r->preview_fd,r->i420,101760,&got)||got!=101760){hide(r);return;}',
       'FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(r->preview_fd,r->i420,r->preview.frame_bytes,&got)||got!=r->preview.frame_bytes){hide(r);return;}')
sub('runtime.c', 'if(r->scene.data&&*(uint32_t*)r->scene.data==320)r->rendered_frames++;',
       'if(r->scene.data&&(*(uint32_t*)r->scene.data==320||*(uint32_t*)r->scene.data==1024))r->rendered_frames++;')
sub('runtime.c', ' if(pb_scene_frame(&r->scene,r->commands,r->pixels,sizeof(r->pixels),r->i420,sizeof(r->i420))!=FX_OK){hide(r);return;}',
       ' if(!r->pixels||pb_scene_frame(&r->scene,r->commands,r->pixels,PB_SCENE_DATA_BYTES,r->i420,r->preview.frame_bytes)!=FX_OK){hide(r);return;}')
sub('runtime.c', 'if(pb_scene_badge_bitmap(&r->scene,r->commands,r->pixels,sizeof(r->pixels))==FX_OK)present(r);',
       'if(r->pixels&&pb_scene_badge_bitmap(&r->scene,r->commands,r->pixels,PB_SCENE_DATA_BYTES)==FX_OK)present(r);')
sub('runtime.c', ' memset(r,0,sizeof(*r));r->magic=PB_MAGIC;r->bytes=sizeof(*r);r->key=9;r->media_epoch=1;r->preview_fd=-1;',
       ''' memset(r,0,sizeof(*r));r->magic=PB_MAGIC;r->bytes=sizeof(*r);r->key=9;r->media_epoch=1;r->preview_fd=-1;
 r->i420=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(PB_PREVIEW_FRAME_BYTES,0);
 r->pixels=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(PB_SCENE_DATA_BYTES,0);
 if(!r->i420||!r->pixels)r->error=0xe003;   /* no preview playback; the rest keeps working */''')
sub('runtime.c', ' char p[64];if(live_sync_file(p,r->native_path,LIVE_FILE_LVP))return 0;',
       ' char p[64];if(!r->i420||!r->pixels||live_sync_file(p,r->native_path,LIVE_FILE_LVP))return 0;')
sub('preview.c', 'if(!w||!h||(w|h)&1||w>640||h>424||!n||n>90||dt<33333||dt>200000)return -1;',
       'if(!w||!h||(w|h)&1||w>1024||h>682||!n||n>90||dt<33333||dt>200000)return -1;')
print('pb full-resolution patch applied to', src)
