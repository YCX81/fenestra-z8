#!/usr/bin/env python3
"""v13 Live changes on top of v12 (hardware 11-07, MAIN 1539a297 on NFP v0.4).
  1. Even frame spacing. The interval gate (stamp - last_pts < sample_us) ran before the duplicate
     check, so with a 30 fps source and 60 Hz live-view interrupts (half-press) an attempt that met
     the interval but found the old buffer was dropped, the next one (new buffer) came 16.7 ms late
     and the one after failed the interval: kept spacing alternated 33/67 ms (~21 fps). Now the
     duplicate check comes first and the interval gate only applies to a new buffer, so every new
     picture is taken at the first interrupt after it appears (30 fps source -> 30 fps, 60 -> 30).
Usage: v13_patch.py SRC_DIR (a copy of src-fix12)"""
import sys
from pathlib import Path
src = Path(sys.argv[1])


def sub(path, old, new, count=1):
    p = src / path; t = p.read_text()
    assert t.count(old) == count, (path, old[:80], t.count(old))
    p.write_text(t.replace(old, new))


C = 'capture/live_stream.c'
GATE = '  if(stamp<=s->last_pts || (s->last_pts && stamp-s->last_pts<s->tune.sample_us)){D->interval++;c->dropped++;goto finish;}\n'
sub(C, GATE, '')
DUP = '  if(s->last_kept_y&&D->y==s->last_kept_y){c->ops.lease_end(c->ops.context,&source);s->dup_skipped++;fr.flags|=4;c->dropped++;goto finish;}\n'
sub(C, DUP, DUP + '  /* v13: the interval gate applies only to a new buffer (after the duplicate check). */\n'
    '  if(stamp<=s->last_pts || (s->last_pts && stamp-s->last_pts<s->tune.sample_us)){c->ops.lease_end(c->ops.context,&source);D->interval++;c->dropped++;goto finish;}\n')
print('v13 Live patch applied to', src)

# 2. Portrait Live (hardware 11-07: a vertical still shows upright in playback, its Live clip plays
#    rotated by 90 degrees). The still's EXIF Orientation (IFD0 0x0112) is read from the JPEG prefix
#    that the transaction already loads, and applied to: the MOV video tkhd matrix (players rotate,
#    as for iPhone portrait clips), the in-camera preview (each LVP frame is rotated upright and fitted
#    454x682 in the 1024x682 frame with black bars, like a vertical still in landscape playback), and
#    the manifest (orientation=N).
H = 'codec/live_codec.h'
sub(H, ' uint8_t atoms[16384];\n} live_mov;', ' uint8_t atoms[16384];\n unsigned orient; /* v13: EXIF orientation of the still, 1 = none */\n} live_mov;')
sub(H, 'int live_jpeg_pair_prefix(uint8_t *prefix,size_t bytes,const char uuid[36]);\n',
    'int live_jpeg_pair_prefix(uint8_t *prefix,size_t bytes,const char uuid[36]);\n'
    '/* v13: EXIF IFD0 Orientation (1..8) of a JPEG prefix, 1 when absent or malformed. */\n'
    'unsigned live_jpeg_orientation(const uint8_t *prefix,size_t bytes);\n')
J = 'codec/live_jpeg.c'
sub(J, 'int live_jpeg_pair_prefix(uint8_t*p,size_t n,const char uuid[36]){',
    'unsigned live_jpeg_orientation(const uint8_t*p,size_t n){\n'
    ' if(!p||n<4||p[0]!=255||p[1]!=216)return 1;\n'
    ' for(size_t at=2;at+4<=n;){if(p[at]!=255)return 1;unsigned marker=p[at+1];if(marker==0xda||marker==0xd9)return 1;\n'
    '  unsigned len=r16(p+at+2,0);if(len<2||len>n-at-2)return 1;\n'
    '  if(marker==0xe1&&len>=16&&!memcmp(p+at+4,"Exif\\0\\0",6)){\n'
    '   uint8_t*t=(uint8_t*)p+at+10;size_t tn=len-8;int le=t[0]==\'I\'&&t[1]==\'I\';if(!le&&!(t[0]==\'M\'&&t[1]==\'M\'))return 1;\n'
    '   const uint8_t*e=entry(t,tn,r32(t+4,le),0x0112,le);if(!e||r16(e+2,le)!=3||r32(e+4,le)!=1)return 1;\n'
    '   unsigned o=r16(e+8,le);return o>=1&&o<=8?o:1;}\n'
    '  at+=len+2;}\n'
    ' return 1;\n}\n'
    'int live_jpeg_pair_prefix(uint8_t*p,size_t n,const char uuid[36]){')
M = 'codec/live_mov.c'
sub(M, 'static uint8_t *matrix(uint8_t*p){static const uint32_t a[9]={65536,0,0,0,65536,0,0,0,0x40000000};for(unsigned i=0;i<9;i++)p=put(p,a[i]);return p;}',
    'static uint8_t *matrix_o(uint8_t*p,unsigned o,unsigned w,unsigned h){uint32_t a[9]={65536,0,0,0,65536,0,0,0,0x40000000};\n'
    ' /* v13: display rotation as in iPhone portrait clips; tx/ty 16.16 */\n'
    ' if(o==6){a[0]=0;a[1]=65536;a[3]=0xffff0000u;a[4]=0;a[6]=h<<16;}\n'
    ' else if(o==8){a[0]=0;a[1]=0xffff0000u;a[3]=65536;a[4]=0;a[7]=w<<16;}\n'
    ' else if(o==3){a[0]=0xffff0000u;a[4]=0xffff0000u;a[6]=w<<16;a[7]=h<<16;}\n'
    ' for(unsigned i=0;i<9;i++)p=put(p,a[i]);return p;}\n'
    'static uint8_t *matrix(uint8_t*p){return matrix_o(p,1,0,0);}')
sub(M, 'static uint8_t *track_header(uint8_t*p,unsigned id,unsigned duration,unsigned width,unsigned height){uint8_t*a=p;p=F(p,(\'t\',\'k\',\'h\',\'d\'));a[11]=3;p=zero(p,8);p=put(p,id);p=zero(p,4);p=put(p,duration);p=zero(p,16);p=matrix(p);',
    'static uint8_t *track_header_o(uint8_t*p,unsigned id,unsigned duration,unsigned width,unsigned height,unsigned o){uint8_t*a=p;p=F(p,(\'t\',\'k\',\'h\',\'d\'));a[11]=3;p=zero(p,8);p=put(p,id);p=zero(p,4);p=put(p,duration);p=zero(p,16);p=matrix_o(p,o,width,height);')
sub(M, 'return end(a,p);}\nstatic uint8_t *media_header(',
    'return end(a,p);}\nstatic uint8_t *track_header(uint8_t*p,unsigned id,unsigned duration,unsigned width,unsigned height){return track_header_o(p,id,duration,width,height,1);}\nstatic uint8_t *media_header(')
sub(M, 'p=track_header(p,1,duration,m->width,m->height);', 'p=track_header_o(p,1,duration,m->width,m->height,m->orient);')
T = 'codec/live_transaction.h'
sub(T, ' void *audio_ctx;uint32_t audio_frames;\n} live_still_ticket;',
    ' void *audio_ctx;uint32_t audio_frames;\n uint32_t *orient_out; /* v13: receives the still\'s EXIF orientation before the preview */\n} live_still_ticket;')
X = 'codec/live_transaction.c'
sub(X, ' if(t->has_jpeg){\n if(t->native_size<4', ' unsigned orient=1;\n if(t->has_jpeg){\n if(t->native_size<4')
sub(X, ' if(readat(f,in,0,w->io,n)||live_jpeg_pair_prefix(w->io,n,t->uuid))goto done;',
    ' if(readat(f,in,0,w->io,n))goto done;\n orient=live_jpeg_orientation(w->io,n);if(t->orient_out)*t->orient_out=orient;\n'
    ' if(live_jpeg_pair_prefix(w->io,n,t->uuid))goto done;')
sub(X, ' if(live_mov_begin(&w->movie,mov_write,&sink_,width,height))goto done;',
    ' if(live_mov_begin(&w->movie,mov_write,&sink_,width,height))goto done;\n w->movie.orient=orient;')
sub(X, 'const char *keys[] = {"native=","\\nsize=","\\nuuid=","\\nvolume_generation="};\n for(unsigned k=0;k<4;k++)',
    'const char *keys[] = {"native=","\\nsize=","\\nuuid=","\\nvolume_generation=","\\norientation="};\n for(unsigned k=0;k<5;k++)')
sub(X, 'else z+=decimal(m+z,t->volume_generation);}', 'else if(k==3)z+=decimal(m+z,t->volume_generation);else z+=decimal(m+z,orient);}')
S = 'capture/live_stream.h'
sub(S, ' uint64_t last_kept_y,dup_skipped;\n};', ' uint64_t last_kept_y,dup_skipped;\n uint32_t preview_orient,preview_pad; /* v13 */\n};')
C2 = 'capture/live_stream.c'
sub(C2, ' memcpy(out,s->preview+(size_t)pick*LIVE_PREVIEW_FRAME,LIVE_PREVIEW_FRAME);return 0;\n}',
    ' const uint8_t *src=s->preview+(size_t)pick*LIVE_PREVIEW_FRAME;unsigned o=s->preview_orient;\n'
    ' if(o!=6&&o!=8&&o!=3){memcpy(out,src,LIVE_PREVIEW_FRAME);return 0;}\n'
    ' /* v13: upright preview. 3: 180 degrees, full frame. 6/8: rotate 90 cw/ccw, fit CWxH centred, black bars. */\n'
    ' enum{W=LIVE_PREVIEW_W,H=LIVE_PREVIEW_H,CW=(H*H/W)&~3u,X0=((W-CW)/2)&~1u};\n'
    ' uint8_t *dy=out,*du=dy+W*H,*dv=du+W*H/4;const uint8_t *sy=src,*su=sy+W*H,*sv=su+W*H/4;\n'
    ' for(unsigned pl=0;pl<3;pl++){unsigned w=pl?W/2:W,h=pl?H/2:H,cw=pl?CW/2:CW,x0=pl?X0/2:X0;\n'
    '  const uint8_t *sp=pl==0?sy:pl==1?su:sv;uint8_t *dp=pl==0?dy:pl==1?du:dv;uint8_t fill=pl?128:16;\n'
    '  for(unsigned y=0;y<h;y++){uint8_t *row=dp+(size_t)y*w;\n'
    '   if(o==3){const uint8_t *sr=sp+(size_t)(h-1-y)*w;for(unsigned x=0;x<w;x++)row[x]=sr[w-1-x];continue;}\n'
    '   unsigned ry=y*w/h; /* rotated image is h wide, w tall: row y of the fit maps to rotated row ry */\n'
    '   for(unsigned x=0;x<w;x++){\n'
    '    if(x<x0||x>=x0+cw){row[x]=fill;continue;}\n'
    '    unsigned rx=(x-x0)*h/cw;if(rx>=h)rx=h-1;if(ry>=w)ry=w-1;\n'
    '    unsigned sx=o==6?ry:w-1-ry,syy=o==6?h-1-rx:rx;row[x]=sp[(size_t)syy*w+sx];}}}\n'
    ' return 0;\n}')
R = 'runtime.c'
sub(R, '.preview_frame=live_stream_preview_frame,.preview_ctx=&r->stream,.preview_scratch=r->stream.preview_scratch};',
    '.preview_frame=live_stream_preview_frame,.preview_ctx=&r->stream,.preview_scratch=r->stream.preview_scratch,\n'
    '  .orient_out=&r->stream.preview_orient};\n r->stream.preview_orient=1;')
print('v13 portrait patch applied')

# 3. Power switch OFF with USB attached -> "Abnormal shutdown" at the next power-on (hardware 11-07).
#    v11 forwards event 7 (BSShutdown) straight to the stock handler while USB is connected, skipping
#    Fenestra's shutdown so a standby keeps USB Debug alive. But the power switch sends the same event and
#    then really powers MAIN down: Fenestra never removed A:\FENESTRA\boot-z8.flag, so the boot guard saw
#    an unclean shutdown. Now, on that path, the Live worker parks the flag (reads it, deletes it) before
#    the stock handler runs; if MAIN is still alive 10 s later it was a standby, and the same bytes are
#    written back so the boot guard keeps protecting this session.
# 4. Breadcrumbs: UI events 7/24/33/34 (with the USB state), photo/movie mode changes, Wave5 hand-backs and
#    flag park/restore go to the Live event log, which the worker saves to A:\LIVEDATA\LIVELOG.BIN
#    (at most every 3 s, only when it changed and no clip is being written); the first save of a boot
#    keeps the previous boot's file as LIVEPREV.BIN, so the events before a crash or reboot survive.
S = 'capture/live_stream.h'
sub(S, 'EV_ICON,EV_MUX_STAGE,EV_DCIM};', 'EV_ICON,EV_MUX_STAGE,EV_DCIM,EV_UI,EV_FLAG,EV_MODE,EV_W5,EV_LOGSAVE};')
RH = 'runtime.h'
sub(RH, ' uint64_t audio_retry_us;\n};',
    ' uint64_t audio_retry_us;\n'
    ' /* v13 (appended): boot-flag park/restore and saved event log */\n'
    ' uint32_t flag_req,flag_n,flag_parks,flag_restores,flag_err,last_photo;\n'
    ' uint32_t log_saved_head,log_saves,log_err,log_pad;\n'
    ' uint64_t flag_t,log_saved_t;\n'
    ' uint8_t flag_buf[256],log_io[512];\n};')
UH = 'ui/live_ui.h'
sub(UH, 'void live_runtime_event(uint32_t event);', 'void live_runtime_event(uint32_t event);\nvoid live_runtime_ui_event(uint32_t event);\nint live_runtime_flag_park(void);')
U = 'ui/live_ui.c'
sub(U, '    if(u&&event==7u&&atomic_load(&u->usb)) {\n        atomic_fetch_add(&u->standby_kept,1u);\n',
    '    if(u&&(event==7u||event==24u||event==33u||event==34u))live_runtime_ui_event(event|atomic_load(&u->usb)<<8);\n'
    '    if(u&&event==7u&&atomic_load(&u->usb)) {\n        atomic_fetch_add(&u->standby_kept,1u);\n'
    '        live_runtime_flag_park(); /* v13: a power-off must not leave the boot flag behind */\n')
R = 'runtime.c'
sub(R, ' struct live_runtime *r=state();if(r)live_encoder_preempt(&r->stream.encoder,client);',
    ' struct live_runtime *r=state();if(r){live_log(&r->stream,now_us(),EV_W5,client,LOAD(&r->stream.job));live_encoder_preempt(&r->stream.encoder,client);}')
sub(R, 'static void mux_worker(void *arg) {',
r'''#define LIVE_FLAG_PATH "A:\\FENESTRA\\boot-z8.flag"
#define LIVE_LOG_PATH "A:\\LIVEDATA\\LIVELOG.BIN"
#define LIVE_PREV_PATH "A:\\LIVEDATA\\LIVEPREV.BIN"
void live_runtime_ui_event(uint32_t event){struct live_runtime *r=state();if(r)live_log(&r->stream,now_us(),EV_UI,event,0);}
/* UI task, event 7 with USB attached: the worker parks the boot flag; wait for it (bounded). */
int live_runtime_flag_park(void){
 struct live_runtime *r=state();if(!r||!LOAD(&r->task_started))return -1;
 STORE(&r->flag_req,1);
 for(unsigned i=0;i<300 && LOAD(&r->flag_req)==1;i++)FN(0x40187b00,int32_t (*)(uint32_t))(1);
 return LOAD(&r->flag_req)==2?0:-1;
}
static void flag_step(struct live_runtime *r,uint64_t now){
 uint32_t q=LOAD(&r->flag_req);
 if(q!=1 && !(q==2 && r->flag_n && now-r->flag_t>10000000u))return;
 live_native_file_context fc={.user=r};live_file_ops f;live_native_file_ops(&f,&fc);int fd=-1;size_t got=0;
 if(q==1){
  r->flag_n=0;
  if(!f.open_read(f.opaque,LIVE_FLAG_PATH,&fd)){
   if(f.read(f.opaque,fd,r->flag_buf,sizeof r->flag_buf,&got))got=0;f.close(f.opaque,fd);
   if(got && got<sizeof r->flag_buf && !f.remove_owned(f.opaque,LIVE_FLAG_PATH)){r->flag_n=(uint32_t)got;r->flag_parks++;}
   else r->flag_err++;
  }
  r->flag_t=now;live_log(&r->stream,now,EV_FLAG,1,r->flag_n);STORE(&r->flag_req,2);return;
 }
 /* Still running 10 s after event 7: a standby, not a power-off. Put the same flag back. */
 uint32_t ok=0;size_t w=0;
 if(!f.create_exclusive(f.opaque,LIVE_FLAG_PATH,&fd)){ok=!f.write(f.opaque,fd,r->flag_buf,r->flag_n,&w)&&w==r->flag_n;if(f.close(f.opaque,fd))ok=0;}
 if(ok)r->flag_restores++;else r->flag_err++;
 live_log(&r->stream,now,EV_FLAG,ok?2:3,r->flag_n);r->flag_n=0;STORE(&r->flag_req,0);
}
/* LIVELOG.BIN: 32-byte header {'LLG1', log head, now_us, parks, restores, flag errors, saves} + the event ring. */
static void log_step(struct live_runtime *r,uint64_t now){
 uint32_t h=LOAD(&r->stream.log_head);
 if(h==r->log_saved_head || now-r->log_saved_t<3000000u || LOAD(&r->stream.job) || LOAD(&r->flag_req)==1)return;
 r->log_saved_t=now;
 live_native_file_context fc={.user=r};live_file_ops f;live_native_file_ops(&f,&fc);int fd=-1,out=-1;size_t n=0,w=0;
 if(!r->log_saves){
  if(f.ensure_dir)f.ensure_dir(f.opaque,"A:\\LIVEDATA");
  f.remove_owned(f.opaque,LIVE_PREV_PATH);
  if(!f.open_read(f.opaque,LIVE_LOG_PATH,&fd)){
   if(!f.create_exclusive(f.opaque,LIVE_PREV_PATH,&out)){
    for(unsigned k=0;k<16;k++){if(f.read(f.opaque,fd,r->log_io,sizeof r->log_io,&n)||!n)break;
     if(f.write(f.opaque,out,r->log_io,n,&w)||w!=n)break;if(n<sizeof r->log_io)break;}
    f.close(f.opaque,out);
   }
   f.close(f.opaque,fd);
  }
 }
 f.remove_owned(f.opaque,LIVE_LOG_PATH);
 uint32_t hd[8]={0x31474c4cu,h,(uint32_t)now,(uint32_t)(now>>32),r->flag_parks,r->flag_restores,r->flag_err,r->log_saves+1};
 int ok=0;
 if(!f.create_exclusive(f.opaque,LIVE_LOG_PATH,&out)){
  ok=!f.write(f.opaque,out,hd,sizeof hd,&w)&&w==sizeof hd&&!f.write(f.opaque,out,r->stream.log,sizeof r->stream.log,&w)&&w==sizeof r->stream.log;
  if(f.close(f.opaque,out))ok=0;
 }
 if(ok){r->log_saves++;r->log_saved_head=h;}else r->log_err++;
}
static void mux_worker(void *arg) {''')
sub(R, '  int photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0;\n',
    '  int photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0;\n'
    '  if((uint32_t)photo+1u!=r->last_photo){r->last_photo=(uint32_t)photo+1u;live_log(&r->stream,w1,EV_MODE,(uint32_t)photo,0);}\n')
sub(R, '  FN(0x40187b00,int32_t (*)(uint32_t))(5);\n  uint64_t w5=now_us();',
    '  flag_step(r,now_us());log_step(r,now_us());\n  FN(0x40187b00,int32_t (*)(uint32_t))(5);\n  uint64_t w5=now_us();')
print('v13 power-off flag + breadcrumb patch applied')

# 5. Breadcrumb fixes from QEMU (n4v13-sbt): the ring of 128 events is flushed by routine events within
#    seconds, and after event 7 the card is no longer written, so the events right before a power-off or
#    crash were lost. Important events now also go to a separate 32-entry crumb ring (saved with the log),
#    and event 7 (any USB state) saves the log synchronously before the stock handler runs.
sub(RH, ' uint8_t flag_buf[256],log_io[512];\n};',
    ' uint8_t flag_buf[256],log_io[512];\n uint32_t crumb_head,crumb_pad;struct live_ev crumbs[32];\n};')
sub(UH, 'int live_runtime_flag_park(void);', 'int live_runtime_flag_park(int park);')
sub(U, '        live_runtime_flag_park(); /* v13: a power-off must not leave the boot flag behind */\n', '')
sub(U, '    if(u&&(event==7u||event==24u||event==33u||event==34u))live_runtime_ui_event(event|atomic_load(&u->usb)<<8);\n',
    '    if(u&&(event==7u||event==24u||event==33u||event==34u))live_runtime_ui_event(event|atomic_load(&u->usb)<<8);\n'
    '    /* v13: save the log now; with USB also park the boot flag (a power-off must not leave it behind) */\n'
    '    if(u&&event==7u)live_runtime_flag_park((int)atomic_load(&u->usb));\n')
sub(R, 'void live_runtime_ui_event(uint32_t event){struct live_runtime *r=state();if(r)live_log(&r->stream,now_us(),EV_UI,event,0);}',
    'static void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b){\n'
    ' live_log(&r->stream,t,type,a,b);uint32_t i=__atomic_fetch_add(&r->crumb_head,1,__ATOMIC_RELAXED)%32u;r->crumbs[i]=(struct live_ev){t,type,a,b};}\n'
    'void live_runtime_ui_event(uint32_t event){struct live_runtime *r=state();if(r)crumb(r,now_us(),EV_UI,event,0);}')
sub(R, 'int live_runtime_flag_park(void){\n struct live_runtime *r=state();if(!r||!LOAD(&r->task_started))return -1;\n STORE(&r->flag_req,1);',
    'int live_runtime_flag_park(int park){\n struct live_runtime *r=state();if(!r||!LOAD(&r->task_started))return -1;\n STORE(&r->flag_req,park?1u:3u);')
sub(R, ' for(unsigned i=0;i<300 && LOAD(&r->flag_req)==1;i++)FN(0x40187b00,int32_t (*)(uint32_t))(1);\n return LOAD(&r->flag_req)==2?0:-1;\n}',
    ' for(unsigned i=0;i<300 && (LOAD(&r->flag_req)==1||LOAD(&r->flag_req)==3);i++)FN(0x40187b00,int32_t (*)(uint32_t))(1);\n return LOAD(&r->flag_req)==2||LOAD(&r->flag_req)==0?0:-1;\n}\n'
    'static void log_step(struct live_runtime *r,uint64_t now,int force);')
sub(R, ' uint32_t q=LOAD(&r->flag_req);\n if(q!=1 && !(q==2 && r->flag_n && now-r->flag_t>10000000u))return;',
    ' uint32_t q=LOAD(&r->flag_req);\n if(q==3){log_step(r,now,1);STORE(&r->flag_req,0);return;}\n if(q!=1 && !(q==2 && r->flag_n && now-r->flag_t>10000000u))return;')
sub(R, '  r->flag_t=now;live_log(&r->stream,now,EV_FLAG,1,r->flag_n);STORE(&r->flag_req,2);return;',
    '  r->flag_t=now;crumb(r,now,EV_FLAG,1,r->flag_n);log_step(r,now,1);STORE(&r->flag_req,2);return;')
sub(R, ' live_log(&r->stream,now,EV_FLAG,ok?2:3,r->flag_n);r->flag_n=0;STORE(&r->flag_req,0);',
    ' crumb(r,now,EV_FLAG,ok?2:3,r->flag_n);r->flag_n=0;STORE(&r->flag_req,0);')
sub(R, '/* LIVELOG.BIN: 32-byte header {\'LLG1\', log head, now_us, parks, restores, flag errors, saves} + the event ring. */\nstatic void log_step(struct live_runtime *r,uint64_t now){\n uint32_t h=LOAD(&r->stream.log_head);\n if(h==r->log_saved_head || now-r->log_saved_t<3000000u || LOAD(&r->stream.job) || LOAD(&r->flag_req)==1)return;',
    '/* LIVELOG.BIN: 32-byte header {\'LLG2\', log head, now_us, parks, restores, flag errors, saves, crumb head}\n * + 32 crumbs (important events) + the 128-event ring, all struct live_ev (24 B). */\nstatic void log_step(struct live_runtime *r,uint64_t now,int force){\n uint32_t h=LOAD(&r->stream.log_head);\n if(!force && (h==r->log_saved_head || now-r->log_saved_t<3000000u || LOAD(&r->stream.job) || LOAD(&r->flag_req)==1))return;')
sub(R, ' uint32_t hd[8]={0x31474c4cu,h,(uint32_t)now,(uint32_t)(now>>32),r->flag_parks,r->flag_restores,r->flag_err,r->log_saves+1};',
    ' uint32_t hd[8]={0x32474c4cu,h,(uint32_t)now,(uint32_t)(now>>32),r->flag_parks,r->flag_restores,r->flag_err,LOAD(&r->crumb_head)};')
sub(R, '  ok=!f.write(f.opaque,out,hd,sizeof hd,&w)&&w==sizeof hd&&!f.write(f.opaque,out,r->stream.log,sizeof r->stream.log,&w)&&w==sizeof r->stream.log;',
    '  ok=!f.write(f.opaque,out,hd,sizeof hd,&w)&&w==sizeof hd&&!f.write(f.opaque,out,r->crumbs,sizeof r->crumbs,&w)&&w==sizeof r->crumbs\n'
    '     &&!f.write(f.opaque,out,r->stream.log,sizeof r->stream.log,&w)&&w==sizeof r->stream.log;')
sub(R, ' struct live_runtime *r=state();if(r){live_log(&r->stream,now_us(),EV_W5,client,LOAD(&r->stream.job));',
    ' struct live_runtime *r=state();if(r){crumb(r,now_us(),EV_W5,client,LOAD(&r->stream.job));')
sub(R, 'live_log(&r->stream,w1,EV_MODE,(uint32_t)photo,0);}', 'crumb(r,w1,EV_MODE,(uint32_t)photo,0);}')
sub(R, '  flag_step(r,now_us());log_step(r,now_us());\n', '  flag_step(r,now_us());log_step(r,now_us(),0);\n')
print('v13 crumbs + synchronous save applied')
sub(R, 'static void live_try_encode(struct live_runtime *r);\n',
    'static void live_try_encode(struct live_runtime *r);\nstatic void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b);\n')

# 6. QEMU n4v13b/c-on: saving whenever the 128-event ring changed meant a delete+create+write on the card
#    every 3 s while Live retried its icon (EV_ICON each loop), and twice live view did not come back after
#    the menu exit (v12 and the first v13 build passed). Background saves now happen only when an important
#    event (crumb) was added, at most every 5 s, not in the first 20 s after boot, and only in photo mode;
#    event 7 still saves synchronously.
sub(R, ' uint32_t h=LOAD(&r->stream.log_head);\n if(!force && (h==r->log_saved_head || now-r->log_saved_t<3000000u || LOAD(&r->stream.job) || LOAD(&r->flag_req)==1))return;',
    ' uint32_t h=LOAD(&r->stream.log_head),ch=LOAD(&r->crumb_head);\n'
    ' if(!force && (ch==r->log_saved_head || now<20000000u || now-r->log_saved_t<5000000u || LOAD(&r->stream.job) || LOAD(&r->flag_req)==1 || r->last_photo!=2))return;')
sub(R, ' if(ok){r->log_saves++;r->log_saved_head=h;}else r->log_err++;', ' if(ok){r->log_saves++;r->log_saved_head=ch;}else r->log_err++;')
print('v13 rare background saves applied')

# 7. Hardware 11-07 (v13 bafde645): the playback UI's manifest parser (pbfix sync.c live_manifest_parse)
#    rejects any unknown key, and one bad .MAN aborts the whole LIVEDATA census, so no Live showed in
#    playback. The manifest keeps its four original keys; orientation lives in the MOV matrix and the LVP.
sub(X, 'const char *keys[] = {"native=","\\nsize=","\\nuuid=","\\nvolume_generation=","\\norientation="};\n for(unsigned k=0;k<5;k++)',
    'const char *keys[] = {"native=","\\nsize=","\\nuuid=","\\nvolume_generation="};\n for(unsigned k=0;k<4;k++)')
print('v13.1 manifest keys restored')
