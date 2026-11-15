#!/usr/bin/env python3
"""v36 -> v37 playback layer edits on a copy of pb-fix36 (exact, count==1 replacements).

1. REC on a Live photo plays the preview stored inside the Live MOV (JPEG frames in
   the trailing free atom). A v36 or older shot that still has an LVP file plays as before.
2. The LIVE badge comes back by itself: 0.7 s after the last key press on the same
   photo, and it is re-presented periodically so a native redraw cannot leave it missing.

usage: patch_pb.py PB_DIR   (PB_DIR already holds the new preview.[ch] and preview_writer.c)"""
import sys
from pathlib import Path

src = Path(sys.argv[1])


def edit(name, pairs):
    path = src / name
    text = path.read_text()
    for old, new in pairs:
        assert text.count(old) == 1, (name, text.count(old), old[:70])
        text = text.replace(old, new)
    path.write_text(text)


SEEK = 'FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))'
READ = 'FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))'
OPEN = 'FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))'
CLOSE = 'FN(0x402dd558,uint32_t(*)(int32_t))'

edit('runtime.h', [
    ('''    uint64_t compose_epoch;
} PbRuntime;''',
     '''    uint64_t compose_epoch;
    /* v37, appended: earlier offsets unchanged. */
    LpClip clip;                 /* preview pictures inside the Live MOV */
    uint8_t *jpeg;LpJpeg *decoder;
    uint32_t clip_mode,decode_errors,badge_heals,reserved37;
    uint64_t last_key_us,badge_present_us;
} PbRuntime;'''),
])
edit('runtime.c', [
    # start(): the MOV's own preview first, then a legacy LVP
    ('''static int start(PbRuntime*r,uint64_t stamp){
 char p[64];if(!r->i420||!r->pixels||live_sync_file(p,r->native_path,LIVE_FILE_LVP))return 0;
 int32_t fd=FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))(p,0,0x20);
 if(fd<0||(uint32_t)fd==0x7fff0000)return 0;
 r->preview_fd=fd;uint64_t n=0,pos=0;uint8_t h[32];uint32_t got=0;
 if(!FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,0,2,&n)||
    !FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,0,0,&pos)||pos||
    FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(fd,h,32,&got)||got!=32||
    lp_preview_open(h,n,&r->preview,LOAD(&r->key_epoch))||
    !((r->preview.width==320&&r->preview.height==212)||(r->preview.width==1024&&r->preview.height==682))){hide(r);return 0;}
 r->preview_size=n;r->start_us=stamp;r->last_frame=UINT32_MAX;r->playing=1;return 1;
}''',
     f'''static int read_at(int32_t fd,uint64_t at,void *out,uint32_t n){{
 uint64_t pos=0;uint32_t got=0;
 return {SEEK}(fd,(int64_t)at,0,&pos)&&pos==at&&!{READ}(fd,out,n,&got)&&got==n;
}}
/* v37: the preview is inside the Live MOV (trailing free atom, JPEG pictures).
 * Shots from v36 and older have a separate LVP file instead; both play. */
static int start(PbRuntime*r,uint64_t stamp){{
 char p[64];if(!r->i420||!r->pixels)return 0;
 uint32_t epoch=LOAD(&r->key_epoch);
 for(unsigned kind=LIVE_FILE_MOV;kind<=LIVE_FILE_LVP;kind++){{
  if((kind==LIVE_FILE_MOV&&!r->jpeg)||live_sync_file(p,r->native_path,kind))continue;
  int32_t fd={OPEN}(p,0,0x20);
  if(fd<0||(uint32_t)fd==0x7fff0000)continue;
  uint64_t n=0,atom=0;uint32_t bytes=0;uint8_t h[32];
  int ok={SEEK}(fd,0,2,&n)!=0;
  if(ok&&kind==LIVE_FILE_MOV){{
   ok=n>8u&&read_at(fd,n-8u,h,8u)&&!lp_clip_tail(h,n,&atom,&bytes);
   uint32_t head=bytes<LP_CLIP_HEAD?bytes:LP_CLIP_HEAD;
   ok=ok&&read_at(fd,atom,r->jpeg,head)&&!lp_clip_open(r->jpeg,head,atom,bytes,&r->preview,&r->clip,epoch);
  }}else if(ok)
   ok=read_at(fd,0,h,32u)&&!lp_preview_open(h,n,&r->preview,epoch)&&
      ((r->preview.width==320&&r->preview.height==212)||(r->preview.width==1024&&r->preview.height==682));
  if(!ok){{{CLOSE}(fd);continue;}}
  r->preview_fd=fd;r->clip_mode=kind==LIVE_FILE_MOV;
  r->preview_size=n;r->start_us=stamp;r->last_frame=UINT32_MAX;r->playing=1;return 1;
 }}
 return 0;
}}'''),
    ('''static void frame(PbRuntime*r,uint64_t stamp){
 uint64_t off=0;int result=lp_preview_frame(&r->preview,stamp-r->start_us,LOAD(&r->key_epoch),&off);
 if(result){hide(r);if(result==1)r->blocked_key_epoch=0;return;}
 uint32_t index=(uint32_t)((off-32)/r->preview.frame_bytes);if(index==r->last_frame)return;
 uint64_t pos=0;uint32_t got=0;
 if(!FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(r->preview_fd,(int64_t)off,0,&pos)||pos!=off||
    FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(r->preview_fd,r->i420,r->preview.frame_bytes,&got)||got!=r->preview.frame_bytes){hide(r);return;}
''',
     '''static void frame(PbRuntime*r,uint64_t stamp){
 uint32_t index=0;int result=lp_preview_index(&r->preview,stamp-r->start_us,LOAD(&r->key_epoch),&index);
 if(result){hide(r);if(result==1)r->blocked_key_epoch=0;return;}
 if(index==r->last_frame)return;
 if(r->clip_mode){
  uint32_t n=(uint32_t)(r->clip.at[index+1u]-r->clip.at[index]);
  if(!read_at(r->preview_fd,r->clip.at[index],r->jpeg,n)||
     lp_jpeg_decode(r->decoder,r->jpeg,n,r->i420,r->preview.width,r->preview.height)){r->decode_errors++;hide(r);return;}
 }else if(!read_at(r->preview_fd,32u+(uint64_t)index*r->preview.frame_bytes,r->i420,r->preview.frame_bytes)){hide(r);return;}
'''),
    # worker: the badge returns after key presses settle
    ('''  if(key!=lastkey){hide(r);r->blocked_key_epoch=key;lastkey=key;}
''',
     '''  if(key!=lastkey){hide(r);r->blocked_key_epoch=key;lastkey=key;}
  /* v37: a key press hides the badge for the native redraw; it used to stay away until
   * another photo was shown. Bring it back once the keys have been quiet for 0.7 s. */
  else if(r->blocked_key_epoch&&!r->playing&&now_us()-r->last_key_us>700000u)r->blocked_key_epoch=0;
'''),
    ('''    if(r->cache_valid&&LOAD(&r->key)==9)start(r,now_us());
''',
     '''    if(r->cache_valid&&LOAD(&r->key)==9&&!start(r,now_us()))r->blocked_key_epoch=0; /* nothing to play: badge back */
'''),
    (''' if(!r->i420||!r->pixels)r->error=0xe003;   /* no preview playback; the rest keeps working */
''',
     ''' if(!r->i420||!r->pixels)r->error=0xe003;   /* no preview playback; the rest keeps working */
 r->jpeg=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(LP_JPEG_MAX+16u+(uint32_t)sizeof(LpJpeg),0);
 if(r->jpeg)r->decoder=(LpJpeg*)(void*)(r->jpeg+LP_JPEG_MAX+16u);   /* else only legacy LVP previews play */
'''),
    (''' if(!rising)return (uint32_t)owns_rec;
''',
     ''' if(!rising)return (uint32_t)owns_rec;
 r->last_key_us=now_us();
'''),
    # UI poll: periodic re-present of an attached badge
    ('''  else if(r->display.display.scene==&r->badge_scene&&(pose!=r->badge_pose||LOAD(&r->badge_blocked)))request=1;
''',
     '''  else if(r->display.display.scene==&r->badge_scene&&(pose!=r->badge_pose||LOAD(&r->badge_blocked)))request=1;
  /* v37: repaint the badge every 0.5 s; a native redraw of the page may have covered it. */
  else if(r->display.display.scene==&r->badge_scene&&now_us()-r->badge_present_us>500000u){r->badge_heals++;request=1;}
'''),
    ('''  if(result==FX_OK){r->badge_pose=pose;r->ui_presented++;''',
     '''  r->badge_present_us=now_us();
  if(result==FX_OK){r->badge_pose=pose;r->ui_presented++;'''),
])
print('patched', src)
