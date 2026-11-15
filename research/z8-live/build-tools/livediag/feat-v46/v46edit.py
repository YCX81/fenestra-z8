from pathlib import Path
R=Path.home()/'v46src/research/z8-live/build-tools/livediag/feat-release/src-fix37'
def sub(f,a,b,n=1):
    p=R/f;s=p.read_text();c=s.count(a);assert c==n,(f,a[:60],c);p.write_text(s.replace(a,b))
sub('capture/live_capture.h','LA_ENCODING, LA_FAILED };','LA_ENCODING, LA_FAILED,\n  LA_WARM /* v46: >= LIVE_WARM_US pre-roll since the last resume: a press gives a shorter clip */ };')
sub('capture/live_capture.c','if(!now || LOAD(&c->phase)!=LA_READY || !LOAD(&c->requested))return 0;',
    'uint32_t p=LOAD(&c->phase);\n    if(!now || (p!=LA_READY && p!=LA_WARM) || !LOAD(&c->requested))return 0;')
sub('capture/live_stream.h','EV_VIEW,EV_PB};','EV_VIEW,EV_PB,EV_RESUME,EV_WARM};')
sub('capture/live_stream.h','#define LIVE_IDR_US 500000u','#define LIVE_IDR_US 500000u\n/* v46: a live-view gap longer than this (menu, playback, monitor off) restarts the pre-roll:\n * a clip never splices frames from before the gap. */\n#define LIVE_RESUME_GAP_US 1000000u\n/* v46: pre-roll that already allows a (shorter) clip */\n#define LIVE_WARM_US 500000u')
s=(R/'capture/live_stream.h').read_text()
i=s.rfind('#endif');s=s[:i]+'/* v46: 0..11 pre-roll twelfths since the last resume, 12 = ready/saving */\nuint32_t live_stream_progress(const struct la_capture *c,const struct live_stream *s,uint64_t now);\n'+s[i:];(R/'capture/live_stream.h').write_text(s)
# resume reset before encoding
sub('capture/live_stream.c',' live_encoder_set_flags(&s->encoder,',
''' if(!job && s->last_pts && stamp>s->last_pts+LIVE_RESUME_GAP_US){
   /* v46: live view came back after a gap: drop the stale pre-roll, keep memory and encoder,
    * and start the new pre-roll with a key frame. */
   live_log(s,now,EV_RESUME,s->ring.count,stamp-s->last_pts);
   au_ring_init(&s->ring,s->ring.data,s->ring.capacity);
   s->preview_head=0;s->preview_count=0;s->preview_last=0;s->last_idr_us=0;STORE(&c->phase,LA_REQUESTED);
  }
  live_encoder_set_flags(&s->encoder,''')
sub('capture/live_stream.c',''' else if(!job){unsigned start;
  if(now>=s->tune.window_us && s->last_pts && now-s->last_pts<=s->tune.ready_fresh_us &&
    !au_ring_find_start(&s->ring,now-s->tune.window_us,&start)){if(c->phase!=LA_READY)live_log(s,now,EV_READY,s->ring.count,0);STORE(&c->phase,LA_READY);}
  else STORE(&c->phase,LA_REQUESTED);''',''' else if(!job){unsigned start;
  int fresh=s->last_pts && now-s->last_pts<=s->tune.ready_fresh_us;
  if(now>=s->tune.window_us && fresh &&
    !au_ring_find_start(&s->ring,now-s->tune.window_us,&start)){if(c->phase!=LA_READY)live_log(s,now,EV_READY,s->ring.count,0);STORE(&c->phase,LA_READY);}
  else if(s->tune.short_start && now>=LIVE_WARM_US && fresh && !au_ring_find_start(&s->ring,now-LIVE_WARM_US,&start)){
   if(c->phase!=LA_WARM)live_log(s,now,EV_WARM,s->ring.count,0);STORE(&c->phase,LA_WARM);}
  else STORE(&c->phase,LA_REQUESTED);''')
p=R/'capture/live_stream.c';s=p.read_text()
s+='''
uint32_t live_stream_progress(const struct la_capture *c,const struct live_stream *s,uint64_t now) {
 uint32_t p=LOAD(&c->phase);
 if(p==LA_READY||(p>=LA_POST&&p<=LA_ENCODING))return 12u;
 if(!s->last_pts||now<s->last_pts||now-s->last_pts>s->tune.ready_fresh_us||!s->tune.window_us)return 0u;
 for(unsigned i=0;i<s->ring.count;i++){const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
  if(e->idr){if(now<=e->pts)return 0u;uint64_t n=(now-e->pts)*12u/s->tune.window_us;return n>11u?11u:(uint32_t)n;}}
 return 0u;
}
''';p.write_text(s)
# runtime
sub('runtime.c',' if(p==LA_READY)v|=LIVE_READY;',' if(p==LA_READY)v|=LIVE_READY;\n if(p==LA_WARM)v|=LIVE_WARM;\n v|=(LOAD(&r->ch2_pad)&15u)<<LIVE_PROGRESS_SHIFT; /* v46: pre-roll progress (twelfths) */')
sub('runtime.c','LOAD(&r->capture.phase)!=LA_READY){r->stream.ignored_presses++;return;}',
    '(LOAD(&r->capture.phase)!=LA_READY && LOAD(&r->capture.phase)!=LA_WARM)){r->stream.ignored_presses++;return;}')
sub('runtime.c','  uint64_t w3=now_us();D->last_step_us=w3-w2;',
    '  STORE(&r->ch2_pad,live_stream_progress(&r->capture,&r->stream,now_us())); /* v46: spare word, pinned ABI */\n  uint64_t w3=now_us();D->last_step_us=w3-w2;')
sub('runtime.h','uint32_t ch2_req,ch2_state,ch2_applies,ch2_restores,ch2_reapplies,ch2_pad,ch2_saved[12]; /* v17 */',
    'uint32_t ch2_req,ch2_state,ch2_applies,ch2_restores,ch2_reapplies,ch2_pad /* v46: pre-roll progress 0..12 */,ch2_saved[12]; /* v17 */')
# ui
sub('ui/live_ui.h','#define LIVE_ERROR 8u','#define LIVE_ERROR 8u\n#define LIVE_WARM 16u            /* v46: a press now gives a shorter clip */\n#define LIVE_PROGRESS_SHIFT 8u   /* v46: bits 8..11 = pre-roll twelfths, 12 = ready */')
sub('ui/live_ui.c','    else if(status&LIVE_READY){LABEL("Live Photo: Ready");}',
    '    else if(status&LIVE_READY){LABEL("Live Photo: Ready");}\n    else if(status&LIVE_WARM){LABEL("Live Photo: Short clip");}')
sub('ui/live_ui.c','static void icon32_compose(UiState *u,uint32_t status,uint32_t phase) {','''/* v46: not ready yet -> same artwork, black outline kept, ink dimmed; the twelve outer arcs
 * light up clockwise from 12 o'clock as the pre-roll fills (ring centre 20,22 in the 110x44 art,
 * arcs at radius 14..17 centred on multiples of 30 degrees). */
#define ICON32_DIM_GAIN 110u
static uint32_t icon32_ring_seg(uint32_t x,uint32_t y) {
    int32_t dx=(int32_t)(2u*x+1u)-40,dy=(int32_t)(2u*y+1u)-44;
    if(x>=40u||dx*dx+dy*dy<26*26)return 12u;
    uint32_t ax=(uint32_t)(dx<0?-dx:dx),ay=(uint32_t)(dy<0?-dy:dy);
    uint32_t j=ay*1000u<268u*ax?0u:ay<ax?1u:ay*1000u<3732u*ax?2u:3u;
    return dx>=0&&dy>=0?j:dx<0&&dy>=0?6u-j:dx<0?6u+j:(12u-j)%12u;
}
static void icon32_state_rgba(const Icon32Asset *a,uint32_t i,uint32_t phase,int busy,uint32_t progress,uint8_t out[4]) {
    if(busy||progress>=12u){icon32_shoot_rgba(a,i,phase,busy,out);return;}
    uint8_t pair=icon32_shoot_pixel(a,i),alpha=pair&15u;
    uint32_t k=icon32_ring_seg(i%a->w,i/a->w);
    uint32_t gain=k<12u&&(k+3u)%12u<progress?255u:ICON32_DIM_GAIN;
    out[0]=out[1]=out[2]=alpha?(uint8_t)(((pair>>4)*gain+alpha/2u)/alpha):0u;
    out[3]=(uint8_t)(alpha*17u);
}
static void icon32_compose(UiState *u,uint32_t status,uint32_t phase) {''')
sub('ui/live_ui.c','''        for(uint32_t i=0u;i<w*h;i++) {
            icon32_shoot_rgba(&asset,i,phase,(status&LIVE_BUSY)!=0,p+4u*i);''','''        uint32_t progress=status&(LIVE_READY|LIVE_BUSY)?12u:(status>>LIVE_PROGRESS_SHIFT)&15u;
        for(uint32_t i=0u;i<w*h;i++) {
            icon32_state_rgba(&asset,i,phase,(status&LIVE_BUSY)!=0,progress,p+4u*i);''')
print('ok')
