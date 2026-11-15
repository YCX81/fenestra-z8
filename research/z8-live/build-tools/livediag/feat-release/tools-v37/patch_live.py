#!/usr/bin/env python3
"""v36 -> v37 Live layer edits on a copy of src-fix36 (exact, count==1 replacements).

1. The in-camera preview goes into the Live MOV (trailing free atom, JPEG frames);
   no LIVEDATA/.LVP is written any more.
2. No second MOV: the DCIM/LIV_nnnn.MOV copy is off by default.
3. The shooting LIVE indicator repaints itself periodically and re-attaches after
   repeated failures, so a native redraw can no longer leave it missing.

usage: patch_live.py SRC_DIR   (SRC_DIR already holds the new codec/preview_writer.[ch])"""
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


edit('codec/live_transaction.c', [
    ('''typedef struct {const live_file_ops*f;int fd;uint64_t at;const live_still_ticket*t;} preview_sink;
static int preview_frame(void*v,uint32_t i,void*p,size_t n){preview_sink*s=v;return s->t->preview_frame(s->t->preview_ctx,i,p,n);}
static int preview_put(void*v,const void*p,size_t n,size_t*done){preview_sink*s=v;if(writeat(s->f,s->fd,s->at,p,n))return -1;s->at+=n;*done=n;return 0;}
static void preview_yield(void*v){preview_sink*s=v;yield(s->f);}
''',
     '''/* v37: preview pictures are appended to the MOV itself (codec/preview_writer.h). */
typedef struct {const live_file_ops*f;int fd;const live_still_ticket*t;} preview_sink;
static int preview_frame(void*v,uint32_t i,void*p,size_t n){preview_sink*s=v;if(stop(s->f))return -1;return s->t->preview_frame(s->t->preview_ctx,i,p,n);}
static int preview_put(void*v,uint64_t at,const void*p,size_t n){preview_sink*s=v;return writeat(s->f,s->fd,at,p,n);}
static void preview_yield(void*v){preview_sink*s=v;yield(s->f);}
'''),
    (''' char jpg[128],mov[128],manifest[128],prev[128];int in=-1,out=-1,video=-1,mf=-1,pv=-1,ownjpg=0,ownmov=0,ownmf=0,ownpv=0,encoding=0,rc=-1;sink sink_;''',
     ''' char jpg[128],mov[128],manifest[128],prev[128];int in=-1,out=-1,video=-1,mf=-1,ownjpg=0,ownmov=0,ownmf=0,encoding=0,rc=-1;sink sink_;'''),
    (''' int r=f->close(f->opaque,video);video=-1;if(r)goto done;
 if(f->encode_event)f->encode_event(f->opaque,11);
 if(t->preview_frame){
  if(stop(f)){rc=-2;goto done;}
  if(!t->preview_scratch||f->create_exclusive(f->opaque,prev,&pv))goto done;ownpv=1;
  preview_sink ps={f,pv,0,t};LivePreviewWriter pw={&ps,preview_frame,preview_put,preview_yield};
  if(live_preview_write(&pw,t->preview_scratch,LIVE_PREVIEW_FRAME_BYTES))goto done;
  r=f->close(f->opaque,pv);pv=-1;if(r)goto done;
  if(f->encode_event)f->encode_event(f->opaque,12);
 }
''',
     ''' if(f->encode_event)f->encode_event(f->opaque,11);
 if(t->preview_frame){
  /* After moov, in the same file: one free atom every MOV reader skips. A failure
   * removes the whole MOV below, so a partial atom is never left on the card. */
  if(stop(f)){rc=-2;goto done;}
  if(!t->preview_scratch)goto done;
  preview_sink ps={f,video,t};LivePreviewWriter pw={&ps,preview_frame,preview_put,preview_yield};uint64_t end=0;
  if(live_preview_write(&pw,t->preview_scratch,LIVE_PREVIEW_FRAME_BYTES,w->io,sizeof(w->io),w->movie.pos,&end))goto done;
  if(f->encode_event)f->encode_event(f->opaque,12);
 }
 int r=f->close(f->opaque,video);video=-1;if(r)goto done;
'''),
    (''' if(pv>=0&&f->close(f->opaque,pv))rc=-1;
''', ''),
    ('''if(ownmov&&f->remove_owned(f->opaque,mov))*cleanup|=2;if(ownpv&&f->remove_owned(f->opaque,prev))*cleanup|=8;}''',
     '''if(ownmov&&f->remove_owned(f->opaque,mov))*cleanup|=2;}'''),
])
edit('codec/live_transaction.h', [
    ('''   cleanup_failed bit1 JPEG / bit2 MOV / bit4 manifest / bit8 preview means own orphan could not be removed.''',
     '''   cleanup_failed bit1 JPEG / bit2 MOV / bit4 manifest means own orphan could not be removed.
   v37: the preview is part of the MOV (no LVP file), so it has no cleanup bit of its own.'''),
    (''' /* Optional in-camera playback preview (PREV.LVP); null skips it. */''',
     ''' /* Optional in-camera playback preview, stored inside the MOV; null skips it. */'''),
])
edit('capture/live_stream.c', [
    ('''s->fr_drop=1;s->dcim_copy=1;s->bitrate''', '''s->fr_drop=1;s->dcim_copy=0 /* v37: one MOV per shot */;s->bitrate'''),
    ('''1,3000000u,1,1000000u,1,1,1,1,3,0u};''', '''1,3000000u,1,1000000u,1,1,1,1,3,400u /* v37 */};'''),
])
edit('capture/live_stream.h', [
    ('''/* Small I420 preview kept alongside the bitstream for in-camera playback
 * (PREV.LVP, see codec/preview_writer.h): 15 fps, 64 slots = 4.3 s, which
 * covers the 3 s clip window. Frozen with the clip: no pushes during mux. */''',
     '''/* I420 preview frames kept alongside the bitstream for in-camera playback.
 * v37 stores them as JPEG pictures inside the Live MOV (codec/preview_writer.h),
 * not as a separate LVP file. Frozen with the clip: no pushes during mux. */'''),
    (''' uint32_t icon_refresh_ms;  /* re-present the Live indicator this often, 0 = only on change */''',
     ''' uint32_t icon_refresh_ms;  /* re-present the Live indicator this often (v37 default 400), 0 = only on change */'''),
    ('''/* PREV.LVP frame source: frame i shows''', '''/* Preview frame source: frame i shows'''),
])
edit('ui/live_ui.c', [
    ('''    _Atomic uint32_t icon_blocked;uintptr_t icon_backing;
} UiState;''',
     '''    _Atomic uint32_t icon_blocked;uintptr_t icon_backing;
    uint32_t icon_fail_streak,icon_heals; /* v37, appended: earlier offsets unchanged */
} UiState;'''),
    ('''    u->display_status=UINT32_MAX;u->icon_retry=0u;u->icon_fallback_until=0u;
}
static void indicator(UiState *u,uint32_t status) {''',
     '''    u->display_status=UINT32_MAX;u->icon_retry=0u;u->icon_fallback_until=0u;u->icon_fail_streak=0u;
}
/* v37: an attachment the native GUI silently dropped (menu, playback, monitor
 * rotation, mode switch) keeps failing to present. After this many consecutive
 * failures it is retired and attached again from scratch. */
#define ICON32_HEAL_AFTER 6u
static void indicator(UiState *u,uint32_t status) {'''),
    ('''    if(r==FX_OK&&!u->display.error) {
        u->display_status=status;u->icon_save_error=saved;u->icon_phase=phase;u->icon_pose=pose;
        u->icon_retry=0u;u->presents++;if(refresh)u->refreshes++;
    } else {
        u->present_fail++;u->icon_retry=1u;
        if(rt)live_log(&rt->stream,t,EV_ICON,status,u->display.error);
        /* Retry on the existing attachment. */
    }''',
     '''    if(r==FX_OK&&!u->display.error) {
        u->display_status=status;u->icon_save_error=saved;u->icon_phase=phase;u->icon_pose=pose;
        u->icon_retry=0u;u->icon_fail_streak=0u;u->presents++;if(refresh)u->refreshes++;
    } else {
        u->present_fail++;u->icon_retry=1u;
        if(rt)live_log(&rt->stream,t,EV_ICON,status,u->display.error);
        /* Retry on the existing attachment; a persistent failure starts over. */
        if(++u->icon_fail_streak>=ICON32_HEAL_AFTER){u->icon_heals++;icon32_unlink_stale(u);}
    }'''),
])
print('patched', src)
