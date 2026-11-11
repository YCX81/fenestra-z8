#!/usr/bin/env python3
"""v37 audio step, applied on top of patch_live.py / patch_pb.py (exact, count==1).

The in-camera preview gets sound: the writer records where the MOV's PCM track is
('LVJA' record at the end of the preview atom), and the playback layer plays it
through the stock audio application service, the way the stock movie player does
(C3.11, static analysis only - never run on hardware, no audio model in QEMU):

  post 0x40d233d0(msg), msg = {link[3], callback, id, payload, ctx}
  0x43180014 open    {u32 rate=0, u32 5, u32 0 (16 bit), u32 1, u32 0, u8 ok@0x14}
  0x4318001a buffer  {u64 pcm, u32 bytes, u32 1, u32 0, u8 1, u8 ok@0x15}
  0x43180015 start   {u32 rate=0, u64 block callback@8, u32 0@0x10, u8 ok@0x14}
  0x43180016 stop    {u8 ok@0}; it also unwinds an open that never started
  PCM memory: the stock fixed audio buffer [0xa031f8bb8], 480000 bytes (2.5 s of
  48 kHz stereo s16le); at most 2.4 s are played, so the driver never wraps.
  Guards: service idle ([0xa031f8d28]==0), audio driver idle ([0xa02d4cb48]==1).

usage: patch_audio.py SRC_DIR PB_DIR"""
import sys
from pathlib import Path

src, pb = Path(sys.argv[1]), Path(sys.argv[2])


def edit(root, name, pairs):
    path = root / name
    text = path.read_text()
    for old, new in pairs:
        assert text.count(old) == 1, (name, text.count(old), old[:70])
        text = text.replace(old, new)
    path.write_text(text)


# ---- Live layer: say where the PCM is ----
edit(src, 'codec/preview_writer.h', [
    (''' *   frames: complete baseline JFIF pictures (4:2:0), back to back
''',
     ''' *   frames: complete baseline JFIF pictures (4:2:0), back to back
 *   optional 'LVJA', u32le pcm offset, u32le pcm frames, u32le 48000:
 *     where the MOV's own 48 kHz stereo s16le track sits in mdat (preview sound)
'''),
    ('''int live_preview_write(const LivePreviewWriter *, void *scratch, size_t scratch_bytes,
                       uint8_t *io, size_t io_bytes, uint64_t base, uint64_t *end);''',
     '''int live_preview_write(const LivePreviewWriter *, void *scratch, size_t scratch_bytes,
                       uint8_t *io, size_t io_bytes, uint64_t base, uint64_t *end,
                       uint32_t audio_offset, uint32_t audio_frames);'''),
])
edit(src, 'codec/preview_writer.c', [
    ('''int live_preview_write(const LivePreviewWriter *w, void *scratch, size_t bytes, uint8_t *io, size_t io_bytes,
                       uint64_t base, uint64_t *end) {''',
     '''int live_preview_write(const LivePreviewWriter *w, void *scratch, size_t bytes, uint8_t *io, size_t io_bytes,
                       uint64_t base, uint64_t *end, uint32_t audio_offset, uint32_t audio_frames) {'''),
    ('''    if (high > at) at = high;
''',
     '''    if (high > at) at = high;
    if (audio_offset && audio_frames) {
        uint8_t a[16] = {'L', 'V', 'J', 'A'};
        le32(a + 4, audio_offset); le32(a + 8, audio_frames); le32(a + 12, 48000);
        if (w->write_at(w->ctx, at, a, sizeof a)) return -1;
        at += sizeof a;
    }
'''),
])
edit(src, 'codec/live_transaction.c', [
    ('''w->io,sizeof(w->io),w->movie.pos,&end))goto done;''',
     '''w->io,sizeof(w->io),w->movie.pos,&end,
     audio&&as.at<=0xffffffffu?(uint32_t)as.at:0u,audio?t->audio_frames:0u))goto done;'''),
])

# ---- playback layer ----
edit(pb, 'preview.h', [
    ('''typedef struct { uint64_t at[LP_CLIP_FRAMES+1u]; } LpClip;   /* file offsets, at[frames] = end */''',
     '''typedef struct { uint64_t at[LP_CLIP_FRAMES+1u],atom,end; } LpClip;   /* file offsets, at[frames] = end of pictures */
/* Optional sound record right after the pictures: "LVJA", pcm offset, frames, 48000. */
int lp_clip_audio(const uint8_t record[16],const LpClip *,uint32_t *offset,uint32_t *frames);'''),
])
edit(pb, 'preview.c', [
    ('''    c->at[n]=at;
''',
     '''    c->at[n]=at;c->atom=atom;c->end=end;
'''),
    ('''
/* ---- baseline JPEG ---- */''',
     '''int lp_clip_audio(const uint8_t r[16],const LpClip *c,uint32_t *offset,uint32_t *frames){
    if(le32(r)!=0x414a564cu||le32(r+12)!=48000u)return -1;
    uint32_t at=le32(r+4),n=le32(r+8);
    if(at<32u||!n||n>48000u*8u||(uint64_t)at+(uint64_t)n*4u>c->atom)return -1;   /* inside the movie part */
    *offset=at;*frames=n;return 0;
}

/* ---- baseline JPEG ---- */'''),
])
edit(pb, 'runtime.h', [
    ('''    uint64_t last_key_us,badge_present_us;
} PbRuntime;''',
     '''    uint64_t last_key_us,badge_present_us;
    /* preview sound through the stock audio application service */
    struct { uint64_t link[3],callback;uint32_t id,pad;void *payload;uint64_t ctx; } audio_msg;
    _Alignas(8) uint8_t audio_payload[0x20];
    _Atomic uint32_t audio_ack;
    uint32_t audio_stage,audio_stop,audio_bytes,audio_errors,audio_plays;
    uint64_t audio_t;
} PbRuntime;'''),
])
SEEK = 'FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))'
edit(pb, 'runtime.c', [
    ('''static void hide(PbRuntime*r){
''',
     '''static void hide(PbRuntime*r){
 if(r->audio_stage)r->audio_stop=1;   /* the worker's audio_step winds the service down */
'''),
    ('''/* v37: the preview is inside the Live MOV (trailing free atom, JPEG pictures).''',
     '''/* ---- preview sound (protocol and addresses: tools-v37/patch_audio.py) ---- */
#define PB_AUDIO_SERVICE (*(volatile uint32_t*)(uintptr_t)UINT64_C(0xa031f8d28))   /* 0 = playback service idle */
#define PB_AUDIO_DRIVER (*(volatile uint32_t*)(uintptr_t)UINT64_C(0xa02d4cb48))    /* 1 = audio driver idle */
#define PB_AUDIO_RING (*(volatile uint64_t*)(uintptr_t)UINT64_C(0xa031f8bb8))      /* stock fixed audio buffer */
#define PB_AUDIO_RING_BYTES 480000u
#define PB_AUDIO_PLAY_BYTES 460800u   /* 2.4 s: stop before the driver wraps the buffer */
static void audio_callback(uint32_t event,void *context){
 (void)context;PbRuntime*r=pb_runtime_get();
 if(r&&event!=0x4318001bu)STORE(&r->audio_ack,event);   /* 0x1b: a block was played, nothing to refill */
}
static void audio_post(PbRuntime*r,uint32_t id){
 memset(&r->audio_msg,0,sizeof(r->audio_msg));
 r->audio_msg.callback=(uintptr_t)audio_callback;r->audio_msg.id=id;r->audio_msg.payload=r->audio_payload;
 STORE(&r->audio_ack,0);r->audio_t=now_us();
 FN(0x40d233d0,void(*)(void*))(&r->audio_msg);
}
/* Copy the clip's PCM into the stock buffer and ask the service to open. */
static void audio_begin(PbRuntime*r,int32_t fd){
 uint8_t record[16];uint32_t at=0,frames=0;
 int silent=!read_at(fd,r->clip.at[r->preview.frames],record,16u)||lp_clip_audio(record,&r->clip,&at,&frames);
#ifdef PB_AUDIO_TEST   /* emulation only: no microphone in QEMU, run the service protocol on silence */
 if(silent){silent=0;frames=0;}
#endif
 if(silent||r->audio_stage||PB_AUDIO_SERVICE||PB_AUDIO_DRIVER!=1u||LOAD(&r->shutdown))return;
 uint64_t base=PB_AUDIO_RING;
 if(base<UINT64_C(0x800000000)||base>UINT64_C(0xa40000000)-PB_AUDIO_RING_BYTES||(base&3u))return;
 uint8_t *ring=(uint8_t*)(uintptr_t)base;uint32_t bytes=frames*4u,done=0;
 if(bytes>PB_AUDIO_PLAY_BYTES)bytes=PB_AUDIO_PLAY_BYTES;
 while(done<bytes){   /* file reads go through our own heap block, never straight into the DMA buffer */
  uint32_t n=bytes-done<LP_JPEG_MAX?bytes-done:LP_JPEG_MAX;
  if(!read_at(fd,(uint64_t)at+done,r->jpeg,n))return;
  memcpy(ring+done,r->jpeg,n);done+=n;
 }
 memset(ring+bytes,0,PB_AUDIO_RING_BYTES-bytes);
 for(uint32_t i=0;i<PB_AUDIO_RING_BYTES;i+=64u)__asm__ volatile("dc cvac, %0"::"r"(ring+i):"memory");
 __asm__ volatile("dsb sy":::"memory");
 memset(r->audio_payload,0,sizeof(r->audio_payload));
 r->audio_payload[4]=5u;r->audio_payload[12]=1u;
 r->audio_bytes=bytes;r->audio_stop=0;r->audio_stage=1u;audio_post(r,0x43180014u);
}
/* One step per worker loop: 1 open, 2 buffer, 3 start (each waits for its reply),
 * 4 playing, 5 stopping, 6 the service never answered - leave it alone for good. */
static void audio_step(PbRuntime*r){
 uint32_t stage=r->audio_stage;if(!stage||stage==6u)return;
 uint64_t t=now_us();uint8_t *p=r->audio_payload;
 if(stage==4u){
  if(!r->audio_stop&&t-r->audio_t<(uint64_t)r->audio_bytes*1000u/192u)return;
 }else{
  if(!LOAD(&r->audio_ack)){if(t-r->audio_t>700000u){r->audio_errors++;r->audio_stage=6u;}return;}
  if(stage==5u){r->audio_stage=0;r->audio_stop=0;return;}
  int ok=stage==2u?p[0x15]:p[0x14];
  if(ok&&!r->audio_stop){
   memset(p,0,sizeof(r->audio_payload));
   if(stage==1u){
    uint64_t ring=PB_AUDIO_RING;uint32_t bytes=PB_AUDIO_RING_BYTES,one=1u;
    memcpy(p,&ring,8u);memcpy(p+8,&bytes,4u);memcpy(p+12,&one,4u);p[0x14]=1u;
    r->audio_stage=2u;audio_post(r,0x4318001au);
   }else if(stage==2u){
    uint64_t callback=(uintptr_t)audio_callback;memcpy(p+8,&callback,8u);
    r->audio_stage=3u;audio_post(r,0x43180015u);
   }else{r->audio_stage=4u;r->audio_t=t;r->audio_plays++;}
   return;
  }
  if(!ok)r->audio_errors++;
 }
 memset(p,0,sizeof(r->audio_payload));r->audio_stage=5u;audio_post(r,0x43180016u);
}
/* v37: the preview is inside the Live MOV (trailing free atom, JPEG pictures).'''),
    ('''  r->preview_fd=fd;r->clip_mode=kind==LIVE_FILE_MOV;
''',
     '''  r->preview_fd=fd;r->clip_mode=kind==LIVE_FILE_MOV;
  if(r->clip_mode)audio_begin(r,fd);
'''),
    ('''   if(r->playing)frame(r,now_us());
''',
     '''   /* picture and sound start together: hold the first frame while the service opens (bounded) */
   if(r->playing&&r->audio_stage>=1u&&r->audio_stage<=3u&&!r->audio_stop)r->start_us=now_us();
   else if(r->playing)frame(r,now_us());
'''),
    ('''  if(lock(r)){live_native_sync_step(&r->journal,&r->sync);unlock(r);}
  yield();''',
     '''  audio_step(r);
  if(lock(r)){live_native_sync_step(&r->journal,&r->sync);unlock(r);}
  yield();'''),
])
print('patched', src, pb)
