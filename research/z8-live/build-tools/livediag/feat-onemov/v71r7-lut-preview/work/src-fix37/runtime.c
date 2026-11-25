/* QEMU-only best-effort integration. Snapshot group A is an assumption on
 * hardware and an emulator-defined observable in this private QEMU build. */
#include "runtime.h"
#include "ui/live_ui.h"
#include "capture/native_capture.h"
#include "codec/live_transaction.h"
#include "codec/native_files.h"
#include "filmsim_runtime.h"
/* Frozen V70/V71 Wave5 EV_DIAG3 & close-diag telemetry hooks. */
void live_encoder_diag_timings(const live_encoder *e,
 uint16_t *avg_in_us,uint16_t *avg_wait_us,uint16_t *avg_out_us,uint16_t *avg_parse_us,
 uint32_t *direct_cnt,uint32_t *retrig_cnt);
uint32_t live_encoder_diag_close(const live_encoder *e);
#define FN(a,t) ((t)(uintptr_t)(a))
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
static struct live_runtime *state(void) {
 struct live_runtime *r=live_ui_engine_get();
 return r && r->magic==LIVE_RUNTIME_MAGIC && r->size==sizeof(*r)?r:0;
}
static uint64_t now_us(void) {
 uint64_t n,f;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(n),"=r"(f));
 return f?n/f*1000000+(n%f)*1000000/f:0;
}
void live_requested_set(uint32_t on) {
 struct live_runtime *r=state();if(!r)return;
 STORE(&r->requested,!!on);la_request(&r->capture,!!on);
 if(on && r->stream.tune.audio)STORE(&r->audio_warm,1u);
}
/* V71: 1 while Live post-roll (job==1) is collecting the 1.5 s post-shutter window. */
int live_runtime_post_roll_active(void) {
 struct live_runtime *r=state();
 return r && LOAD(&r->stream.job)==1u;
}
uint32_t live_status(void) {
 struct live_runtime *r=state();if(!r)return LIVE_ERROR;
 uint32_t p=LOAD(&r->capture.phase),v=LOAD(&r->requested)?LIVE_REQUESTED:0;
 if(!LOAD(&r->audio_warm)) {
  if(p==LA_READY)v|=LIVE_READY;
  if(p==LA_WARM)v|=LIVE_WARM;
  v|=(LOAD(&r->ch2_pad)&15u)<<LIVE_PROGRESS_SHIFT;
 } else if(!r->audio_error && r->audio.active) {
  /* V71.3: show filling 0..11 ring during healthy warm-up; stay 0 on audio error. */
  uint32_t prog=LOAD(&r->ch2_pad)&15u;
  if(prog>11u)prog=11u;
  v|=prog<<LIVE_PROGRESS_SHIFT;
 }
 if(p==LA_POST || p==LA_FROZEN || p==LA_ENCODING ||
    (LIVE_TWIN->magic==LIVE_TWIN_MAGIC && LOAD(&LIVE_TWIN->state)==2u))v|=LIVE_BUSY;
 if(LOAD(&r->task_error) || p==LA_FAILED)v|=LIVE_ERROR;
 return v;
}
void live_runtime_event(uint32_t event) {
 struct live_runtime *r=state();if(!r)return;
 /* v71.6/v71.7 hotfix: ev=16 (BSWaitEnableSuspend) fires during normal cold boot
  * NonReleasePhase (~1.75s on real Z8) — NEVER latch shutdown or stop FilmSim! */
 if(event==24u){live_requested_set(0);__atomic_add_fetch(&r->card_epoch,1,__ATOMIC_RELAXED);la_cancel(&r->capture);filmsim_runtime_on_card_change();}
}
void live_irq_notify(uint32_t event,uint64_t status) {
 struct live_runtime *r=state();if(r)la_irq_signal(&r->capture,event,status,now_us());
}
void live_press_notify(void) {
 struct live_runtime *r=state();if(!r||!livephoto_is_module_loaded())return;
 live_shutter_diag(0u,0u);
 __atomic_add_fetch(&r->full_presses,1,__ATOMIC_RELAXED);
 live_log(&r->stream,now_us(),EV_PRESS,LOAD(&r->capture.phase),LOAD(&r->stream.job));
 if(LOAD(&r->audio_warm) || LOAD(&r->stream.job) ||
    (LIVE_TWIN->magic==LIVE_TWIN_MAGIC && LOAD(&LIVE_TWIN->state)==2u) ||
    (LOAD(&r->capture.phase)!=LA_READY && LOAD(&r->capture.phase)!=LA_WARM)){r->stream.ignored_presses++;return;}
 /* Publish the review gate before the press mailbox. ARMING cannot be
  * cleared by the worker, including its exchange(mailbox,0)->job1 gap. */
 uint32_t idle=0;
 if(!__atomic_compare_exchange_n(&r->review_gate,&idle,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
  r->stream.ignored_presses++;return;
 }
 if(la_full_press(&r->capture,now_us())){
  STORE(&r->press_ticket_floor,LOAD(&r->ticket_generation));
  STORE(&r->review_gate,2u);
 }else STORE(&r->review_gate,0u);
}
/* Only the native auto-review slot1c consumer at 0x40407f84 is redirected here.
 * Leave Nikon's pending flag and payload untouched while our post-roll is
 * still being collected. Manual playback transitions never use this hook. */
uint32_t live_review_pop(uint32_t slot,void **payload) {
 struct live_runtime *r=state();
 if(slot==0x1cu && r && LOAD(&r->review_gate)){
  uint32_t setting=FN(0x403ec398,uint32_t (*)(uint32_t))(0x02000408u);
  if(setting==1u || setting==2u){r->review_deferrals++;return 0;}
 }
 return FN(0x403deca8,uint32_t (*)(uint32_t,void **))(slot,payload);
}
/* Sole Live worker, after stream_step and clip_audio. A frozen video still
 * awaiting its final PCM must not lose RX to the native picture-review view.
 * Rejected/cancelled/failed clips also release review through this path. */
static __attribute__((noinline)) void live_review_step(struct live_runtime *r) {
 if(LOAD(&r->review_gate)!=2u || LOAD(&r->capture.press_mailbox) ||
    LOAD(&r->stream.job)==1u || LOAD(&r->audio_phase)==1u)return;
 uint32_t active=2u;
 (void)__atomic_compare_exchange_n(&r->review_gate,&active,0u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static void live_try_encode(struct live_runtime *r);
static void live_twin_step(struct live_runtime *r);
static void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b);
/* Same current-view ABI as the playback module: native getters 40393f6c /
 * 40393f28 read +54 / +d0. View 2 owns both stock playback and REC preview.
 * Do not wait for Wave5 bring-up to stop the microphone: REC never calls it. */
static int live_playback_view(void) {
 uintptr_t p=*(volatile uintptr_t *)(uintptr_t)UINT64_C(0xa032280c8);
 if((p&7u) || p<UINT64_C(0xa00000000) || p>UINT64_C(0xa40000000)-0x110u)return 0;
 return *(volatile uint32_t *)(p+0x54u)==2u;
}
/* Optional FilmSim class-0x24 coordination (enabled when linked with FilmSim). */
#ifndef LIVE_FILMSIM_POOL_HOOKS
#define filmsim_runtime_pool_suspend() ((void)0)
#define filmsim_runtime_pool_resume()  ((void)0)
#endif
/* Stock Wave5 bring-up hook (0x402f2d58), caller task: give Wave5 back to movie/playback.
 * crumb_pad is a pinned spare word: 0=running, 1=stock hold, 2=handoff pending,
 * 3=worker released its stream block. The worker alone owns capture.descriptor. */
__attribute__((section(".live_extra"),noinline,minsize))
void live_w5_bringup_notify(uint32_t client) {
 struct live_runtime *r=state();if(!r)return;
 {
  uint32_t dc0=live_encoder_diag_close(&r->stream.encoder);
  uint64_t wb=(uint64_t)(LOAD(&r->stream.job)&0xffffu)
   |((uint64_t)(dc0&0xffffu)<<16)
   |((uint64_t)(dc0&0xffff0000u)<<16);
  crumb(r,now_us(),EV_W5,client,wb);
 }
 if(client==4u)return;
 filmsim_runtime_pool_suspend();
 /* Keep a second stock caller from retaining a pointer into the pool that the
  * first caller is about to free. This lock is cached runtime RAM, not W5 pool RAM. */
 uint32_t unlocked=0u;uint64_t lock_t=now_us();
 while(!__atomic_compare_exchange_n(&r->stock_handoff_lock,&unlocked,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
  unlocked=0u;
  if(now_us()-lock_t>=6000000u){crumb(r,now_us(),EV_MODE,6u,0);return;}
  FN(0x40187b00,int32_t (*)(uint32_t))(2);
 }
 /* Inhibit the next worker step before preempt drops its release_req handshake.
  * V71.3 Defect 3A: immediately abort any pending pre-mux job (2 -> 4) so
  * mux_worker never opens SD files during handoff and worker frees Class 0x24
  * (< 50 ms) without hitting the timeout.
  * V71.5: widen wait window to 3.0 s; on timeout NEVER overwrite crumb_pad back
  * to 1u (keep 2u persistently until worker drains and live_deferred_dispose
  * completes dispose + 3->1), and still force-dispose encoder if worker already
  * released video buffers. */
 uint64_t loops0=LOAD(&r->stream.diag.loops);
 STORE(&r->crumb_pad,2u);
 if(live_stream_abort_pending_job(&r->stream))
  live_log(&r->stream,now_us(),EV_MUX_ABORT,client,2u);
 live_encoder_preempt(&r->stream.encoder,client);
 uint64_t t0=now_us();
 while(LOAD(&r->task_started) && LOAD(&r->crumb_pad)==2u && now_us()-t0<3000000u)
  FN(0x40187b00,int32_t (*)(uint32_t))(1);
 /* An initial encoder allocation may have entered before hold=2 and published
  * hw after the first preempt. ACK guarantees that path is now finished. */
 int dispose_rc=-7;
 int acked=(LOAD(&r->crumb_pad)==3u);
 int enc_quiet=acked || (LOAD(&r->stream.diag.loops)>=loops0+2u &&
                         !r->capture.descriptor && !LOAD(&r->stream.job) &&
                         LOAD(&r->capture.phase)==LA_OFF);
 if(enc_quiet){
  live_encoder_preempt(&r->stream.encoder,client);
  /* The worker has stopped touching the encoder and every stream buffer.
   * Only a clean native close permits returning the two persistent pool blocks. */
  dispose_rc=live_encoder_dispose(&r->stream.encoder);
 }
 /* b: low32=pending, bits 32..39=-dispose_rc, bits 40..47=eos_miss, bits 48..55=close_fail_stage. */
 uint32_t pending=LOAD(&r->crumb_pad)|(!!r->audio.active<<8)|
  (!!r->audio.descriptor<<9)|(LOAD(&r->audio_z8.phase)<<16);
 uint32_t dc1=live_encoder_diag_close(&r->stream.encoder);
 uint64_t mb=(uint64_t)pending
  |((uint64_t)((uint32_t)(-dispose_rc)&0xffu)<<32)
  |((uint64_t)(dc1&0xffu)<<40)
  |((uint64_t)((dc1>>24)&0xffu)<<48);
 crumb(r,now_us(),EV_MODE,dispose_rc==0?3u:4u,mb);
 /* Start the resume epoch after the hook wait. Only downgrade 3 -> 1 on clean
  * ACK; on timeout keep crumb_pad==2u so live_encode_cancel() stays latched. */
 STORE(&r->vpad,(uint32_t)(now_us()/1000u));
 if(acked)STORE(&r->crumb_pad,1u);
 STORE(&r->stock_handoff_lock,0u);
}
static __attribute__((section(".live_extra"),noinline,minsize)) void live_handoff_if_pool(uint32_t tag) {
 filmsim_runtime_pool_suspend();
 struct live_runtime *r=state();
 if(!r)return;
 if(live_encoder_has_pool(&r->stream.encoder) ||
    r->capture.descriptor || r->stream.fr_desc || LOAD(&r->stream.job) ||
    (LIVE_TWIN->magic==LIVE_TWIN_MAGIC && LOAD(&LIVE_TWIN->state)==2u)){
  r->w5_mirror.handoffs++;
  live_w5_bringup_notify(tag);
 } else {
  /* Even when Live is OFF / holds no pool, latch hold=1 so worker cannot race
   * BE_LV clearing and call filmsim_runtime_pool_resume() prematurely. */
  uint32_t expected=0u;
  STORE(&r->vpad,(uint32_t)(now_us()/1000u));
  (void)__atomic_compare_exchange_n(&r->crumb_pad,&expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
 }
}
/* v69/v71.3 HangFix Layer 1 + 1B: layout 0 -> non-0 switch (0x40f7a4bc / 0x40f7679c /
 * 0x407c5f60 / 0x40712808) AND PBPRCMXM Command 46 layout 0 subflag 0 -> 1 subpool carve
 * (0x407c5ed8, carving 646.81 MiB subpools 0x5a,0x5c..0x5f out of Class 0x24). Strictly
 * gated on BE_LV active-channel bitmask 0xa03b71788 == 0 so Photo LV is never touched. */
__attribute__((section(".live_extra"),minsize,noinline))
uint32_t live_layout_switch_wrap(const void *req) {
 if(req &&
    *(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03f31a68)==0u &&
    *(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03b71788)==0u) {
  uint32_t new_layout=*(const uint32_t *)req;
  uint8_t new_sub=((const uint8_t *)req)[4];
  uint8_t cur_sub=*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03f31a6c);
  if(new_layout!=0u || (new_sub!=0u && cur_sub==0u))
   live_handoff_if_pool(0xfeu);
 }
 return FN(0x410463e4,uint32_t (*)(const void *))(req);
}
/* v71.5 HangFix Layer 1C: 0x40f81680 (BL 0x41046ef8 -> 0x41045e08) carves 647.55 MiB
 * subpools 0x5a..0x5f directly from Class 0x24 bypassing 0x410463e4. Placed in .text
 * (live_primary has >5 KiB free; .live_extra is tight). */
uint32_t live_subpool_carve_wrap(void);
__attribute__((noinline,minsize))
uint32_t live_subpool_carve_wrap(void) {
 if(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03f31a68)==0u &&
    *(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03f31a6c)==0u &&
    *(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03b71788)==0u)
  live_handoff_if_pool(0xfbu);
 return FN(0x41046ef8,uint32_t (*)(void))();
}
/* v71.5 PB Bounce Fix (Mechanism B): while already in Photo Playback (view=2,
 * owner=1) and pb_state0 is still blocked waiting for PhotoDevelop/DCF before
 * any playback page has mounted (0x403dbdb4() == NULL), swallow KEY_PLAYBACK on
 * arbiter slot 0x2b (0x42ac31b8 -> 0x403917e8) so pressing Playback immediately
 * after shutter (Image Review ON or double-press) does not cancel Playback and
 * bounce back to Shooting LV. Half-press shutter uses a separate arbiter slot
 * and remains instant. */
uint32_t live_pb_arbiter_exit_wrap(void *ctx);
__attribute__((noinline,minsize))
uint32_t live_pb_arbiter_exit_wrap(void *ctx) {
 uintptr_t v=*(volatile uintptr_t *)(uintptr_t)UINT64_C(0xa032280c8);
 if(!(v&7u) && v>=UINT64_C(0xa00000000) && v<=UINT64_C(0xa40000000)-0x110u &&
    *(volatile uint32_t *)(v+0x54u)==2u && *(volatile uint32_t *)(v+0xd0u)==1u &&
    !FN(0x403dbdb4,uintptr_t (*)(void))())
  return 0u;
 return FN(0x403917e8,uint32_t (*)(void *))(ctx);
}
/* v69 HangFix Layer 2A: MOVS playback buffer allocator (0x40711da4 -> 0x4104d264,
 * immediately before 0x40711da8 0x41046f84->0x41047014 & 0x40711db8 0x410463e4). */
__attribute__((section(".live_extra"),minsize,noinline))
void live_movs_alloc_wrap(void *out) {
 live_handoff_if_pool(0xfdu);
 FN(0x4104d264,void (*)(void *))(out);
}
/* v69 HangFix Layer 2B: ProRes HW bring-up entry (0x40cb0f34, REC & PB). */
__attribute__((section(".live_extra"),minsize,noinline))
void live_prores_bringup_notify(void) { live_handoff_if_pool(0xfcu); }
/* Both paths release worker-owned capture/audio storage before acknowledgement
 * or resume. The retained Wave5 pool is separate from these descriptors. */
static __attribute__((noinline,minsize)) int live_stock_buffers_quiet(const struct live_runtime *r) {
 return !r->capture.descriptor && !r->stream.fr_desc && !LOAD(&r->stream.job) &&
        !(LIVE_TWIN->magic==LIVE_TWIN_MAGIC && LOAD(&LIVE_TWIN->state)==2u) &&
        !r->audio.active && !r->audio.descriptor && LOAD(&r->audio_z8.phase)==LAZ8_IDLE &&
        !LOAD(&r->shutter.phase);
}
static __attribute__((minsize)) uint32_t live_stock_hold(struct live_runtime *r,int photo,uint64_t tn) {
 uint32_t hold=LOAD(&r->crumb_pad);
 int playback=live_playback_view();
 if(playback && !hold){
  uint32_t expected=0u;
  STORE(&r->vpad,(uint32_t)(tn/1000u));
  if(__atomic_compare_exchange_n(&r->crumb_pad,&expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
   hold=1u;crumb(r,tn,EV_MODE,5u,0);
  }else hold=expected;
 }
 if(hold==1u){
  if(playback)STORE(&r->vpad,(uint32_t)(tn/1000u));
  else if(photo && !*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03f31a68) &&
          !*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03f31a6c) &&
          !LOAD(&r->shutdown) && live_stock_buffers_quiet(r)){
   /* This lock also excludes a stock hook disposing the published W5 pool.
    * Ordinary REC uses software preview and retains its existing native W5
    * instance. A native codec caller still requires preempt/dispose (hold=2). */
   uint32_t unlocked=0u;
   if(__atomic_compare_exchange_n(&r->stock_handoff_lock,&unlocked,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
    if(LOAD(&r->crumb_pad)==1u){
     int ready=!*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d980) ||
               live_encoder_retained(&r->stream.encoder);
     if(!ready)STORE(&r->vpad,(uint32_t)(tn/1000u));
     else {
      uint64_t lvh=LOAD(&r->capture.irq_timestamp);uint32_t epoch=LOAD(&r->vpad);
      if(lvh && (int32_t)((uint32_t)(lvh/1000u)-epoch)>0 &&
         (uint32_t)(tn/1000u)-epoch>50u){
       uint32_t expected=1u;
       if(__atomic_compare_exchange_n(&r->crumb_pad,&expected,0u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        crumb(r,tn,EV_MODE,2u,0);
      }
     }
    }
    STORE(&r->stock_handoff_lock,0u);
   }
   hold=LOAD(&r->crumb_pad);
  }
 }
 return hold;
}
/* An explicit native codec caller receives ACK only after worker storage and
 * both audio owners are quiet. A photo-only REC hold does not power W5 down. */
extern int live_ui_pb_audio_handoff(void);
__attribute__((section(".live_extra"),noinline))
static void live_stock_release_ack(struct live_runtime *r) {
 uint32_t expected=2u;
 if(LOAD(&r->crumb_pad)!=2u || !live_ui_pb_audio_handoff() ||
    !live_stock_buffers_quiet(r))return;
 (void)__atomic_compare_exchange_n(&r->crumb_pad,&expected,3u,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
/* DMA callback context (audio-driver task): registry read only. */
struct live_audio_z8 *live_audio_z8_context(void) {
 struct live_runtime *r=state();return r?&r->audio_z8:0;
}
/* Worker task only. Full-duplex audio follows the video clip without pausing
 * RX during shutter TX: mark at press, freeze [clip_start, clip_end) once
 * video froze, release after mux. Any audio failure leaves the clip video-only. */
static __attribute__((noinline)) void live_audio_step(struct live_runtime *r,int want,uint64_t now) {
 struct live_audio *a=&r->audio;
 uint32_t job=LOAD(&r->stream.job),phase=LOAD(&r->audio_phase);
 if(live_shutter_step(r,want,now))goto clip_audio;
 if(!want || (!r->stream.tune.soft_cancel && la_cancelled(&r->capture)) || (a->active && (LOAD(&a->cancelled) || r->audio_error))) {
  int hard_cancel=LOAD(&r->shutdown)||(a->active&&(LOAD(&a->cancelled)||r->audio_error))||(!r->stream.tune.soft_cancel&&la_cancelled(&r->capture));
  if(!hard_cancel && job==2u && phase==1u) {
   struct live_audio_window w={r->stream.clip_start,r->stream.clip_end};
   struct live_audio_info info;uint32_t got=0;
   if((!live_audio_freeze(a,w) || !live_audio_freeze_best(a,w)) &&
      !live_audio_get(a,0,0,0,&got,&info) && info.frames) {
    r->audio_info=info;phase=2u;STORE(&r->audio_phase,2u);
   }
  }
  if(phase==2u && (job==2u||job==3u))return; /* preserve readable frozen PCM until mux ends */
  live_audio_cancel(a);
  if(a->active)live_audio_stop(a);
  STORE(&r->audio_phase,0);return;
 }
 if(!a->active) {
  if(now<r->audio_retry_us)return;
  int e=live_audio_start(a);
  if(e){
   uint32_t tries=(r->audio_error>>8)+1u;if(tries>255u)tries=255u;
   r->audio_error=((uint32_t)(-e)&0xffu)|(tries<<8);r->audio_retry_us=now+200000u;return;
  }
  r->audio_starts++;r->audio_error=0;
 }
 int e=live_audio_z8_poll(a,&r->audio_z8);
 if(e && e!=LAUDIO_GAP){
  uint32_t tries=(r->audio_error>>8)+1u;if(tries>255u)tries=255u;
  r->audio_error=((uint32_t)(-e)&0xffu)|(tries<<8);r->audio_retry_us=now+200000u;
  if(!LOAD(&r->shutter.phase))live_audio_stop(a);
  return;
 }
 if(r->audio_z8.unmuted)r->audio_error=0;
 clip_audio:
 if(job==1u && !phase)
  STORE(&r->audio_phase,live_audio_mark(a,r->stream.clip_press)?3u:1u);
 else if(job==2u && phase==1u) {
  struct live_audio_window w={r->stream.clip_start,r->stream.clip_end};
  int f=live_audio_freeze(a,w);
  if(f==LAUDIO_GAP){
   uint64_t lo=(w.begin_us/1000000u)*LIVE_AUDIO_RATE+(w.begin_us%1000000u)*LIVE_AUDIO_RATE/1000000u+(w.begin_us%125u!=0u);
   if(a->first_index>lo || now>r->stream.clip_end+400000u)f=live_audio_freeze_best(a,w);
  }
  struct live_audio_info info;uint32_t got=0;
  if(!f && !live_audio_get(a,0,0,0,&got,&info) && info.frames){r->audio_info=info;STORE(&r->audio_phase,2u);}
  else if(f!=LAUDIO_GAP || now>r->stream.clip_end+1500000u){r->audio_skipped++;STORE(&r->audio_phase,3u);}
 }
 else if(job==2u && !phase)STORE(&r->audio_phase,3u);
 else if((job==0u||job==4u) && phase){live_audio_thaw(a);STORE(&r->audio_phase,0);}
}
/* Worker-only, published to the UI/press hook. Duplex TX does not suspend RX
 * readiness/error observation. Keep Preparing during brief startup retries. */
static __attribute__((noinline)) void live_audio_ready_step(struct live_runtime *r,uint64_t now) {
 uint32_t warm=0;struct live_audio *a=&r->audio;
 if(r->stream.tune.audio && LOAD(&r->requested) && (a->active || (a->ops.verified_photo_backend && (r->audio_error>>8)<15u) || !r->audio_error)) {
  unsigned start;warm=1;
  uint64_t need=r->stream.tune.short_start?LIVE_WARM_US:r->stream.tune.window_us;
  if(!r->audio_error && r->audio_z8.unmuted && now>=need &&
     !au_ring_find_start(&r->stream.ring,now-need,&start)) {
   uint64_t begin=r->stream.ring.entries[(r->stream.ring.head+start)%AU_RING_SLOTS].pts;
   warm=!live_audio_covers(a,begin,now,2u*LAZ8_BLOCK_US);
  }
 }
 STORE(&r->audio_warm,warm);
}
static int live_audio_ticket_get(void *ctx,uint32_t off,int16_t *out,uint32_t cap,uint32_t *got) {
 struct live_runtime *r=ctx;struct live_audio_info info;
 int e=live_audio_get(&r->audio,off,out,cap,got,&info);
 /* got is a count of stereo frames, not int16_t samples. Apply the v28
  * +12 dB gain to both channels throughout the complete returned block. */
 if(!e&&got)for(uint32_t i=0;i<*got*LIVE_AUDIO_CHANNELS;i++){int32_t s=(int32_t)out[i]*4;out[i]=(int16_t)(s>32767?32767:s<-32768?-32768:s);} /* v50 */
 return e;
}
static size_t live_audio_ticket_trak(void *ctx,uint8_t *out,size_t cap,uint64_t at) {
 struct live_runtime *r=ctx;
 return live_audio_mov_trak(out,cap,&r->audio_info,3,1000000,at);
}
#define LIVE_FLAG_PATH "A:\\FENESTRA\\boot-z8.flag"
/* Diagnostic log on the card the core selected for FENESTRA (card-slot word
 * 0xa1171b000: magic 'CSL1', drive 1 = A, 2 = B); A when the word is absent. */
#define CARD_SLOT_MAGIC (*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa1171b000))
#define CARD_SLOT_DRIVE (*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa1171b004))
static char log_drive(void){return CARD_SLOT_MAGIC==0x314c5343u&&CARD_SLOT_DRIVE==2u?'B':'A';}
static void on_drive(char *p,const char *s){p[0]=log_drive();unsigned i=1;while(s[i]){p[i]=s[i];i++;}p[i]=0;}
#define LIVE_LOG_PATH_A "A:\\LIVEDATA\\LIVELOG.BIN"
#define LIVE_PREV_PATH_A "A:\\LIVEDATA\\LIVEPREV.BIN"
static void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b){
 live_log(&r->stream,t,type,a,b);uint32_t i=__atomic_fetch_add(&r->crumb_head,1,__ATOMIC_RELAXED)%32u;r->crumbs[i]=(struct live_ev){t,type,a,b};}

/* Passive shutter evidence: RAM reads + existing crumbs only. Native sound
 * decisions and arguments are untouched; card I/O stays in log_step's worker.
 * EV60 a: point/incoming/prepare-state/fire-state (four bytes).
 * b: driver/service/sound-running/stock-mute/inhibit/prepared/capture/active. */
__attribute__((section(".live_extra"),noinline))
void live_shutter_diag(uint32_t point,uint32_t incoming){
 struct live_runtime *r=state();if(!r)return;
 const volatile uint8_t *sound=(const volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc7008);
 uint32_t a=(point&255u)|((incoming&255u)<<8)
  |((*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa0292be18)&255u)<<16)
  |((*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa0292be1c)&255u)<<24);
 uint64_t b=(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa02d4cb48)&255u)
  |((uint64_t)(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa031f8d28)&255u)<<8)
  |((uint64_t)sound[8]<<16)|((uint64_t)sound[0]<<24)|((uint64_t)sound[14]<<32)
  |((uint64_t)*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc4f38)<<40)
  |((uint64_t)(LOAD(&r->capture.phase)&255u)<<48)|((uint64_t)(r->audio.active&255u)<<56);
 crumb(r,now_us(),60u,a,b);
}

void live_runtime_ui_event(uint32_t event){struct live_runtime *r=state();if(r)crumb(r,now_us(),EV_UI,event,0);}
/* UI task, event 7 with USB attached: the worker parks the boot flag; wait for it (bounded). */
int live_runtime_flag_park(int park){
 struct live_runtime *r=state();if(!r||!LOAD(&r->task_started))return -1;
 STORE(&r->flag_req,park?1u:3u);
 for(unsigned i=0;i<300 && (LOAD(&r->flag_req)==1||LOAD(&r->flag_req)==3);i++)FN(0x40187b00,int32_t (*)(uint32_t))(1);
 return LOAD(&r->flag_req)==2||LOAD(&r->flag_req)==0?0:-1;
}
static __attribute__((noinline,minsize)) void log_step(struct live_runtime *r,uint64_t now,int force);
static __attribute__((noinline,minsize)) void flag_step(struct live_runtime *r,uint64_t now){
 uint32_t q=LOAD(&r->flag_req);
 if(q==3 && !r->flag_n){STORE(&r->flag_req,0);return;}
 if(q!=1 && q!=3 && !(q==2 && r->flag_n && now-r->flag_t>10000000u))return;
 live_native_file_context fc={.user=r};live_file_ops f;live_native_file_ops(&f,&fc);int fd=-1;size_t got=0;
 if(q==1){
  if(!r->flag_n){
   if(!f.open_read(f.opaque,LIVE_FLAG_PATH,&fd)){
    if(f.read(f.opaque,fd,r->flag_buf,sizeof r->flag_buf,&got))got=0;f.close(f.opaque,fd);
    if(got && got<sizeof r->flag_buf && !f.remove_owned(f.opaque,LIVE_FLAG_PATH)){r->flag_n=(uint32_t)got;r->flag_parks++;}
    else r->flag_err++;
   }
  }
  r->flag_t=now;crumb(r,now,EV_FLAG,1,r->flag_n);if(livephoto_is_module_loaded())log_step(r,now,1);STORE(&r->flag_req,2);return;
 }
 /* Restore parked BOOT-Z8.FLAG on ev=8/15 (q==3) or 10 s after ev=7 (USB standby). */
 uint32_t ok=0;size_t w=0;
 if(!f.create_exclusive(f.opaque,LIVE_FLAG_PATH,&fd)){ok=!f.write(f.opaque,fd,r->flag_buf,r->flag_n,&w)&&w==r->flag_n;if(f.close(f.opaque,fd))ok=0;}
 if(ok)r->flag_restores++;else r->flag_err++;
 crumb(r,now,EV_FLAG,ok?2:3,r->flag_n);r->flag_n=0;STORE(&r->flag_req,0);
}
/* Cached RMW locks in unused picker_time fields (zero ABI change). */
static __attribute__((noinline,minsize)) void twin_lock(struct live_runtime *r){uint32_t z=0;while(!__atomic_compare_exchange_n(&r->picker_time.channel,&z,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){z=0;FN(0x40187b00,int32_t (*)(uint32_t))(0);}}
static void twin_unlock(struct live_runtime *r){STORE(&r->picker_time.channel,0u);}
/* LIVELOG.BIN: 32-byte header {'LLG2', log head, now_us, parks, restores, flag errors, saves, crumb head}
 * + 32 crumbs (important events) + the 128-event ring, all struct live_ev (24 B). */
static __attribute__((noinline,minsize)) void log_step(struct live_runtime *r,uint64_t now,int force){
 if(!force && (now<20000000u || now-r->log_saved_t<5000000u || LOAD(&r->stream.job) || LOAD(&r->flag_req) || r->last_photo!=2))return;
 uint64_t lt=force?now_us():0;
 for(uint32_t z=0;!__atomic_compare_exchange_n(&r->picker_time.flags,&z,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);z=0){
  if(!force||now_us()-lt>200000u)return;
  FN(0x40187b00,int32_t (*)(uint32_t))(1);
 }
 live_ui_evf_flush(r);
 uint32_t h=LOAD(&r->stream.log_head),ch=LOAD(&r->crumb_head);
 if(!force && ch==r->log_saved_head){STORE(&r->picker_time.flags,0u);return;}
 r->log_saved_t=now;
 live_native_file_context fc={.user=r};live_file_ops f;live_native_file_ops(&f,&fc);int fd=-1,out=-1;size_t n=0,w=0;
 char LIVE_LOG_PATH[32],LIVE_PREV_PATH[32],dir[16];on_drive(LIVE_LOG_PATH,LIVE_LOG_PATH_A);on_drive(LIVE_PREV_PATH,LIVE_PREV_PATH_A);on_drive(dir,"A:\\LIVEDATA");
 if(!r->log_saves){
  if(f.ensure_dir)f.ensure_dir(f.opaque,dir);
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
 uint32_t hd[8]={0x32474c4cu,h,(uint32_t)now,(uint32_t)(now>>32),r->flag_parks,r->flag_restores,r->flag_err,LOAD(&r->crumb_head)};
 int ok=0;
 if(!f.create_exclusive(f.opaque,LIVE_LOG_PATH,&out)){
  ok=!f.write(f.opaque,out,hd,sizeof hd,&w)&&w==sizeof hd&&!f.write(f.opaque,out,r->crumbs,sizeof r->crumbs,&w)&&w==sizeof r->crumbs
     &&!f.write(f.opaque,out,r->stream.log,sizeof r->stream.log,&w)&&w==sizeof r->stream.log;
  if(f.close(f.opaque,out))ok=0;
 }
 if(ok){r->log_saves++;r->log_saved_head=ch;}else r->log_err++;
 STORE(&r->picker_time.flags,0u);
}
/* Retired v14-v23 debug MMIO/view probes; struct fields remain ABI-reserved. */
static void mux_worker(void *arg) {
 struct live_runtime *r=arg;STORE(&r->stream.mux_started,1);
 for(;;){
  live_ui_poll();__atomic_add_fetch(&r->poll_count,1u,__ATOMIC_RELAXED);
  int lp_loaded=livephoto_is_module_loaded();
  int force_log=__atomic_exchange_n(&r->log_force_req,0u,__ATOMIC_ACQ_REL)!=0u;
  if(lp_loaded) {
   live_try_encode(r);live_twin_step(r);
   log_step(r,now_us(),force_log);
  } else {
   if(live_stream_abort_pending_job(&r->stream))
    live_log(&r->stream,now_us(),EV_MUX_ABORT,0x444fu,0);
   if(force_log)
    log_step(r,now_us(),1);
  }
  FN(0x40187b00,int32_t (*)(uint32_t))(5);
 }
}
static __attribute__((section(".live_extra"),noinline,minsize)) void live_deferred_dispose(struct live_runtime *r,int photo,uint32_t hold) {
 uint32_t gate=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03f31a68)|
               *(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03f31a6c);
 int playback=live_playback_view();
 if(photo && !gate && !hold && !playback){
  if(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03b71788))
   filmsim_runtime_pool_resume();
  return;
 }
 if(hold!=1u && hold!=3u)return;
 if(hold==1u && ((photo && !gate && !playback) || !r->w5_mirror.magic || r->w5_mirror.close_failed))return;
 uint32_t unlocked=0u;
 if(!__atomic_compare_exchange_n(&r->stock_handoff_lock,&unlocked,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
 uint32_t cur=LOAD(&r->crumb_pad);
 if((cur==1u || cur==3u) && live_stock_buffers_quiet(r)){
  if(r->w5_mirror.magic && !r->w5_mirror.close_failed){
   if(live_encoder_retained(&r->stream.encoder))
    live_encoder_preempt(&r->stream.encoder,0xfau);
   if(!*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d980))
    (void)live_encoder_dispose(&r->stream.encoder);
  }
  if(cur==3u){
   STORE(&r->vpad,(uint32_t)(now_us()/1000u));
   STORE(&r->crumb_pad,1u);
  }
 }
 STORE(&r->stock_handoff_lock,0u);
}
static void worker(void *arg) {
 struct live_runtime *r=arg;STORE(&r->task_started,1);
 for(;;) {
  struct live_diag *D=&r->stream.diag;uint64_t w0=now_us();
  if(D->loop_t0){D->last_loop_us=w0-D->loop_t0;if(D->last_loop_us>D->max_loop_us)D->max_loop_us=D->last_loop_us;}
  D->loop_t0=w0;D->loops++;
  uint64_t w1=now_us();D->last_ui_us=w1-w0;if(D->last_ui_us>D->max_ui_us)D->max_ui_us=D->last_ui_us;
  /* PHOTO is the only implemented mode gate; other release modes remain
   * unvalidated. Keep the worker outside the native shooting path. */
  int photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0;
  if((uint32_t)photo+1u!=r->last_photo){r->last_photo=(uint32_t)photo+1u;crumb(r,w1,EV_MODE,(uint32_t)photo,0);}
  /* V71.7-r4: Strict 0 B EL1 dormant gating on livephoto.wlm (0x401ccde4).
   * Preserves live_stock_hold(), live_deferred_dispose()->filmsim_runtime_pool_resume(),
   * and flag_step() (USB BOOT-Z8.FLAG park/restore) unconditionally. */
  int lp_loaded=livephoto_is_module_loaded();
  if(!lp_loaded){
   if(LOAD(&r->requested))live_requested_set(0u);
   if(live_stream_abort_pending_job(&r->stream))
    live_log(&r->stream,now_us(),EV_MUX_ABORT,0x444fu,1u);
  }
  uint64_t w2=now_us();
  uint32_t hold=live_stock_hold(r,photo,now_us());
  if(hold==2u && live_stream_abort_pending_job(&r->stream))
   live_log(&r->stream,now_us(),EV_MUX_ABORT,2u,2u);
  uint64_t at=now_us(),alv=LOAD(&r->capture.irq_timestamp);
  int audio_want=lp_loaded && r->stream.tune.audio && photo && !hold && !LOAD(&r->shutdown) &&
   LOAD(&r->requested) && ((alv && at>=alv && at-alv<UINT64_C(1500000)) || LOAD(&r->stream.job));
  /* C0's soft cancellation must first be acknowledged by stream_step, as in
   * v52/v63. Otherwise an early audio call could stop RX if soft_cancel==0. */
  int audio_need_step=lp_loaded || r->audio.active || r->audio.descriptor ||
   LOAD(&r->audio_phase) || LOAD(&r->shutter.phase);
  if(audio_need_step && !la_cancelled(&r->capture))live_audio_step(r,audio_want,at);
  live_audio_ready_step(r,at);
  int eligible=lp_loaded && photo && !LOAD(&r->shutdown) && !hold;
  int may_release=!eligible || !LOAD(&r->requested) || D->geometry_since ||
                  (!r->stream.tune.soft_cancel && LOAD(&r->capture.cancel_epoch)!=r->capture.worker_epoch);
  int twin_busy=0;
  if(may_release && LIVE_TWIN->magic==LIVE_TWIN_MAGIC){
   twin_lock(r);
   uint32_t ts=LOAD(&LIVE_TWIN->state);
   if(ts==1u)STORE(&LIVE_TWIN->state,0u);
   else if(ts==2u)twin_busy=1;
   twin_unlock(r);
  }
  if(LOAD(&r->stream.job)==1u && LIVE_TWIN->magic==LIVE_TWIN_MAGIC && LOAD(&LIVE_TWIN->state)==2u)twin_busy=1;
  int skip_stream=twin_busy || hold==3u ||
   (!eligible && !r->capture.descriptor && !LOAD(&r->stream.job) && LOAD(&r->capture.phase)==LA_OFF);
  if(!skip_stream) {
   uint64_t enc0=r->stream.encoded,idx0=r->stream.cfr_index,drop0=r->stream.copy_dropped;
   live_stream_step(&r->capture,&r->stream,now_us(),eligible,now_us);
   if(eligible && LOAD(&r->requested)) {
    /* V70: drain overdue CFR ticks (e.g. post-preemption / post-shutter catch-up)
     * without spinning when idle or on encoder BUSY/copy rejection. */
    for(unsigned cu=0;cu<3u;cu++) {
     if(r->stream.copy_dropped!=drop0 || (r->stream.cfr_index==idx0 && r->stream.encoded==enc0))break;
     if(!r->stream.cfr_have || LOAD(&r->stream.job)>1u)break;
     uint64_t next_tick=r->stream.cfr_epoch+r->stream.cfr_index*1001000u/30u;
     if(now_us()<next_tick)break;
     enc0=r->stream.encoded;idx0=r->stream.cfr_index;drop0=r->stream.copy_dropped;
     live_stream_step(&r->capture,&r->stream,now_us(),1,now_us);
    }
   }
  }
  if((hold || !lp_loaded) && !r->capture.descriptor && !LOAD(&r->stream.job))
   live_stream_release_aux(&r->capture,&r->stream);
  live_deferred_dispose(r,photo,hold);
  if(!lp_loaded && live_encoder_has_pool(&r->stream.encoder) && live_stock_buffers_quiet(r)) {
   uint32_t unlocked=0u;
   if(__atomic_compare_exchange_n(&r->stock_handoff_lock,&unlocked,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
    if(live_stock_buffers_quiet(r) && live_encoder_has_pool(&r->stream.encoder)) {
     live_encoder_preempt(&r->stream.encoder,0xf9u);
     if(!*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d980))
      (void)live_encoder_dispose(&r->stream.encoder);
    }
    STORE(&r->stock_handoff_lock,0u);
   }
  }
  STORE(&r->ch2_pad,lp_loaded?live_stream_progress(&r->capture,&r->stream,now_us()):0u); /* v46: spare word, pinned ABI */
  uint64_t w3=now_us();D->last_step_us=w3-w2;if(D->last_step_us>D->max_step_us)D->max_step_us=D->last_step_us;
  uint64_t t=now_us(),lv=LOAD(&r->capture.irq_timestamp);
  int lv_on=lv && t>=lv && t-lv<UINT64_C(1500000);
  if(lp_loaded || r->audio.active || r->audio.descriptor || LOAD(&r->audio_phase) || LOAD(&r->shutter.phase))
   live_audio_step(r,lp_loaded && r->stream.tune.audio && photo && !hold && !LOAD(&r->shutdown) && LOAD(&r->requested) && (lv_on || LOAD(&r->stream.job)),t);
  live_review_step(r);
  live_audio_ready_step(r,now_us());
  /* Audio stop is asynchronous; an idle video descriptor alone is not ACK. */
  live_stock_release_ack(r);
  uint64_t w4=now_us();D->last_audio_us=w4-t;if(D->last_audio_us>D->max_audio_us)D->max_audio_us=D->last_audio_us;
  /* Keep flag_step on worker: idle is a single atomic load (<10 ns), while USB
   * shutdown flag_park must never wait behind a multi-second mux/twin save. */
  flag_step(r,now_us());
  uint32_t cur_job=LOAD(&r->stream.job);
  int twin_active=(LIVE_TWIN->magic==LIVE_TWIN_MAGIC && LOAD(&LIVE_TWIN->state)==2u);
  int fast_pace=eligible && LOAD(&r->requested) && cur_job<=1u && !twin_active;
  uint32_t sleep_ms=(!lp_loaded && live_stock_buffers_quiet(r) && !live_encoder_has_pool(&r->stream.encoder) && LOAD(&r->crumb_pad)!=2u)
   ? 20u : (fast_pace?0u:(LOAD(&r->crumb_pad)==2u?1u:5u));
  FN(0x40187b00,int32_t (*)(uint32_t))(sleep_ms);
  uint64_t w5=now_us();D->last_sleep_us=w5-w4;if(D->last_sleep_us>D->max_sleep_us)D->max_sleep_us=D->last_sleep_us;
 }
}
void live_runtime_boot(void) {
 if(state())return;
 struct live_runtime *r=FN(0x40d2c408,void *(*)(uint32_t,uint32_t))(sizeof(*r),0);
 if(!r)return;
 for(unsigned i=0;i<sizeof(*r);i++)((uint8_t *)r)[i]=0;
 r->magic=LIVE_RUNTIME_MAGIC;r->size=sizeof(*r);r->lv_channel=1;
 struct la_ops ops={0};
 ops.context=&r->capture;ops.allocate=la_native_allocate;ops.release=la_native_release;
 ops.lease_begin=la_best_begin;ops.lease_valid=la_best_valid;ops.lease_end=la_best_end;
 /* These callbacks are best effort and do not establish a DMA lease. */
 la_init(&r->capture,&ops);live_stream_init(&r->stream);
 live_audio_z8_init(&r->audio_z8,0);
 struct live_audio_ops audio_ops;live_audio_z8_ops(&r->audio_z8,&audio_ops);
 live_audio_init(&r->audio,&audio_ops);
 live_ui_engine_set(r);
 r->stream.mux_stack_raw=FN(0x40d2c408,void *(*)(uint32_t,uint32_t))(65536+16,0);
 if(!r->stream.mux_stack_raw){STORE(&r->task_error,2);return;}
 struct la_native_task *mt=(void *)r->stream.mux_scope;
 /* v67: place both worker (prio 29) and mux_worker (prio 31) on CPU 0 (sel=1):
  * avoids CPU 2 AF-C/CV tasks (24 tasks at prio 2..28, table 0xa029c19e0/2f18)
  * and pb_worker (30), keeps worker below DISPLAY(10)/GUISRV(26)/APPLV(28) and
  * mux_worker below EXMCUSRV/EYEDEV/TPL(30), and shares CPU 0 cache cleanly. */
 if(!la_native_task_start(mt,mux_worker,r,
 (void *)(((uintptr_t)r->stream.mux_stack_raw+15)&~(uintptr_t)15),65536,31,1,1)){
  STORE(&r->task_error,mt->native_error?(uint32_t)mt->native_error:2u);return;}

 r->worker_stack_raw=FN(0x40d2c408,void *(*)(uint32_t,uint32_t))(65536+16,0);
 if(!r->worker_stack_raw){STORE(&r->task_error,1);return;}
 struct la_native_task *task=(struct la_native_task *)r->task_scope;
 if(!la_native_task_start(task,worker,r,
   (void *)(((uintptr_t)r->worker_stack_raw+15)&~(uintptr_t)15),65536,29,1,1))
  STORE(&r->task_error,task->native_error?(uint32_t)task->native_error:1u);
}
static void live_ticket_open(struct live_runtime *,void *);
uint32_t live_open_wrap(void *file) {
 uint32_t result=FN(0x4085b320,uint32_t (*)(void *))(file);
 struct live_runtime *r=state();
 if(r && result==0x04320080 && file) live_ticket_open(r,file);
 if(r && LOAD(&r->writer_active)==1 && result==0x04320080 && file) {
  const uint8_t *p=file;uint32_t n=*(const uint32_t *)(p+0x94);
  uint32_t id=*(const uint32_t *)(p+0x28);
  if(n>0 && n<32 && p[0x74+n]==0 && id==r->writer_file_id) {
   for(unsigned i=0;i<32;i++)r->writer_path[i]=(char)p[0x74+i];
   r->writer_path_valid=1;
  }
 }
 return result;
}
extern uint32_t live_old_writer(uint32_t,uint32_t,const void *);
uint32_t live_writer_wrap(uint32_t mode,uint32_t format,const void *record) {
 struct live_runtime *r=state();uint32_t expected=0;int owned=0;
 if(r && mode==0 && format==1 && record &&
    __atomic_compare_exchange_n(&r->writer_active,&expected,2,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {
  owned=1;r->writer_file_id=*(const uint32_t *)record;r->writer_path_valid=0;
  STORE(&r->writer_active,1);
 }
 uint32_t result=live_old_writer(mode,format,record);
 if(r){__atomic_add_fetch(&r->writer_returns,1,__ATOMIC_RELAXED);}
 if(owned) {
  r->writer_result=result;live_log(&r->stream,now_us(),EV_WRITER,result,r->writer_path_valid);
  if(result==0 && r->writer_path_valid) {
   for(unsigned i=0;i<32;i++)r->last_native_path[i]=r->writer_path[i];
   r->jpeg_returns++;
  }
  STORE(&r->writer_active,0);
 }
 return result;
}
void live_epoch_notify(uintptr_t base) {
 struct live_runtime *r=state();if(r && base==0x15431000){la_cancel(&r->capture);live_log(&r->stream,now_us(),EV_C0,0,0);}
}
void live_pool_notify(uintptr_t pool) {
 struct live_runtime *r=state();if(r && pool==UINT64_C(0xa03b6b538)){la_cancel(&r->capture);live_log(&r->stream,now_us(),EV_POOL,0,0);}
}
/* Hooks never wait. Contention or ambiguous identities discard this ticket. */
static int ticket_lock(struct live_runtime *r){uint32_t z=0;return __atomic_compare_exchange_n(&r->ticket_lock,&z,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);}
static void ticket_unlock(struct live_runtime *r){STORE(&r->ticket_lock,0);}
static void live_ticket_open(struct live_runtime *r,void *file) {
 const uint8_t *p=file;uint32_t fd=*(const uint32_t *)(p+12),id=*(const uint32_t *)(p+0x28),n=*(const uint32_t *)(p+0x94);
 uint32_t disk=fd>>16,slot=fd&0xffff;
 if(disk>1||slot>=30||!n||n>=32||p[0x74+n])return;
 uint64_t d=UINT64_C(0xa049d55b0)+(uint64_t)disk*0x3f570+(uint64_t)slot*0x21c8;
 if(!ticket_lock(r)){__atomic_add_fetch(&r->ticket_drops,1,__ATOMIC_RELAXED);return;}
 struct live_ticket *t=0;
 /* Native file slots are recycled: a closed ticket on this descriptor is an
  * earlier file a pending Live may still need, so only an in-flight one is reused.
  * A full table evicts the oldest ticket opened before the current Live press. */
 for(unsigned i=0;i<8;i++)if(r->tickets[i].open && !r->tickets[i].closed && r->tickets[i].descriptor==d){t=&r->tickets[i];break;}
 if(!t)for(unsigned i=0;i<8;i++)if(!r->tickets[i].open){t=&r->tickets[i];break;}
 if(!t)for(unsigned i=0;i<8;i++){struct live_ticket *q=&r->tickets[i];
  if(q->generation<=LOAD(&r->press_ticket_floor) && (!t || q->generation<t->generation))t=q;}
 if(t){*t=(struct live_ticket){0};t->descriptor=d;t->fd=fd;t->file_id=id;t->card_epoch=LOAD(&r->card_epoch);t->generation=++r->ticket_generation;
  for(unsigned i=0;i<32;i++)t->path[i]=(char)p[0x74+i];t->open=1;}
 else r->ticket_drops++;
 ticket_unlock(r);
}
uint32_t live_finalize_wrap(void *file,uint64_t size) {
 struct live_runtime *r=state();
 if(r && file && ticket_lock(r)) {
  uint32_t fd=*(uint32_t *)((uint8_t *)file+12);
  uint32_t id=*(uint32_t *)((uint8_t *)file+0x28),epoch=LOAD(&r->card_epoch),count=0;
  struct live_ticket *match=0;
  /* A closed JPEG remains pending while its fd can already belong to the
   * same shot's NEF. Never overwrite that JPEG's length with the later file.
   * Match the live file identity, and reject ambiguous ownership. */
  for(unsigned i=0;i<8;i++) {
   struct live_ticket *q=&r->tickets[i];
   if(q->open && !q->closed && q->fd==fd && q->file_id==id && q->card_epoch==epoch){match=q;count++;}
  }
  if(count==1)match->size=size;else r->ticket_drops++;
  ticket_unlock(r);
 }
 return FN(0x4085b6d4,uint32_t (*)(void *,uint64_t))(file,size);
}
uint32_t live_event_wrap(uint32_t event,void *payload) {
 uint32_t result=FN(0x406bd954,uint32_t (*)(uint32_t,void *))(event,payload);
 struct live_runtime *r=state();if(!r||event!=0x04180002||!payload)return result;
 if(!ticket_lock(r)){__atomic_add_fetch(&r->ticket_drops,1,__ATOMIC_RELAXED);return result;}
 uint32_t id=*(uint32_t *)payload;struct live_ticket *match=0;unsigned count=0;
 for(unsigned i=0;i<8;i++)if(r->tickets[i].open && r->tickets[i].file_id==id && r->tickets[i].card_epoch==LOAD(&r->card_epoch)){match=&r->tickets[i];count++;}
 if(count==1){match->success_us=now_us();match->success=1;}else r->ticket_drops++;
 ticket_unlock(r);return result;
}
void live_closed_notify(uintptr_t descriptor) {
 struct live_runtime *r=state();if(!r)return;
 if(!ticket_lock(r)){__atomic_add_fetch(&r->ticket_drops,1,__ATOMIC_RELAXED);return;}
 for(unsigned i=0;i<8;i++)if(r->tickets[i].open && r->tickets[i].descriptor==descriptor && r->tickets[i].card_epoch==LOAD(&r->card_epoch))r->tickets[i].closed=1;
 ticket_unlock(r);
}
static int live_encode_cancel(void *p){struct live_runtime *r=p;return !LOAD(&r->capture.requested)||LOAD(&r->shutdown)||LOAD(&r->crumb_pad)==2u;}
static int live_twin_cancel(void *p){struct live_runtime *r=p;return live_encode_cancel(p)||LOAD(&r->crumb_pad)>=1u||LOAD(&r->stream.job)!=0u||LOAD(&r->capture.press_mailbox)!=0u;}
static void live_encode_yield(void *p){
 struct live_runtime *r=p;
 if(r){live_ui_poll();__atomic_add_fetch(&r->poll_count,1u,__ATOMIC_RELAXED);}
 la_native_yield();
}
static void live_encode_event(void *p,unsigned event) {
 struct live_runtime *r=p;
 if(event>=10){live_log(&r->stream,now_us(),EV_MUX_STAGE,event-10,now_us()-r->encode_started_us);return;}
 if(event==0){r->frame_encode_completed=0;r->frame_encode_finished_us=0;r->frame_encode_started_us=now_us();}
 else if(event==1)r->frame_encode_completed++;
 else if(event==2)r->frame_encode_finished_us=now_us();
}
/* DCIM/<f>/DSC_nnnn.JPG + LIVE/<f>/DSC_nnnn.MOV -> DCIM/<f>/LIV_nnnn.MOV (stock MTP lists DCIM only). */
static int live_dcim_copy(const live_file_ops *f,const char *still,const char *mov,uint8_t *buf,size_t cap){
 char dst[128];size_t n=0;while(still[n]&&n<120)n++;if(n<12||!buf||!cap)return 2;
 for(size_t i=0;i<=n;i++)dst[i]=still[i];
 size_t b=n-12;dst[b]='L';dst[b+1]='I';dst[b+2]='V';dst[b+3]='_';dst[n-3]='M';dst[n-2]='O';dst[n-1]='V';
 int in=-1,out=-1,rc=0;if(f->open_read(f->opaque,mov,&in))return 3;
 if(f->create_exclusive(f->opaque,dst,&out)){f->close(f->opaque,in);return 4;}
 for(;;){if(f->cancelled&&f->cancelled(f->opaque)){rc=8;break;}size_t got=0,w=0;if(f->read(f->opaque,in,buf,cap,&got)){rc=5;break;}if(!got)break;
  if(f->write(f->opaque,out,buf,got,&w)||w!=got){rc=6;break;}if(got<cap)break;if(f->yield)f->yield(f->opaque);}
 f->close(f->opaque,in);if(f->close(f->opaque,out)&&!rc)rc=7;if(rc)f->remove_owned(f->opaque,dst);return rc;}
/* Second card: the twin JPG has the same folder/stem on the other volume and was opened
 * after the same press. Wait (bounded) until the native writer has closed it. */
static void live_twin_step(struct live_runtime *r) {
 struct live_twin *tw=LIVE_TWIN;
 if(tw->magic!=LIVE_TWIN_MAGIC||LOAD(&tw->state)!=1)return;
 if(LOAD(&r->stream.job)||LOAD(&r->capture.press_mailbox))return;   /* a new clip goes first */
 if(LOAD(&r->card_epoch)!=tw->epoch||LOAD(&r->shutdown)||LOAD(&r->crumb_pad)>=1u||now_us()>tw->deadline_us){
  twin_lock(r);if(LOAD(&tw->state)==1u){STORE(&tw->state,0u);tw->timeouts++;}twin_unlock(r);return;
 }
 if(!ticket_lock(r))return;
 struct live_ticket t={0},*hit=0;char other=tw->source[0]=='A'?'B':'A';
 for(unsigned i=0;i<8;i++){struct live_ticket *q=&r->tickets[i];
  if(!q->open||q->generation<=tw->floor||q->card_epoch!=tw->epoch||q->path[0]!=other)continue;
  unsigned k=1;while(k<32&&q->path[k]==tw->source[k]&&q->path[k])k++;
  if(k<32&&(q->path[k]||tw->source[k]))continue;
  hit=q;break;}
 if(hit&&hit->success&&hit->closed&&hit->size){t=*hit;hit->open=0;}
 ticket_unlock(r);if(!t.open)return;
 twin_lock(r);
 void *work=LOAD(&r->stream.work);
 uint8_t *fb=LOAD(&r->stream.frozen_bytes);
 if(LOAD(&tw->state)!=1u||!work||!fb||live_twin_cancel(r)){if(LOAD(&tw->state)==1u)STORE(&tw->state,0u);twin_unlock(r);return;}
 STORE(&tw->state,2u);
 twin_unlock(r);
 live_native_file_context fc={.user=r,.cancelled=live_twin_cancel,.yield=live_encode_yield};live_file_ops ops;live_native_file_ops(&ops,&fc);
 unsigned cleanup=0;
 /* V71.5: reuse 64B-aligned 512 KiB slice of idle frozen_bytes (8 MiB, disjoint from i420/ring). */
 uint8_t *twin_io=(uint8_t *)(((uintptr_t)fb+63u)&~(uintptr_t)63u);
 int rc=live_write_twin_io(work,&ops,tw->source,tw->mov_bytes,t.path,t.size,tw->uuid,t.card_epoch,twin_io,524288u,&cleanup);
 for(unsigned i=0;i<32;i++)tw->path[i]=t.path[i];
 tw->result=(uint32_t)(-rc);tw->cleanup=cleanup;if(rc)tw->errors++;else tw->copies++;
 live_log(&r->stream,now_us(),EV_MUX_END,0x100u|(uint32_t)(-rc),(uint64_t)fc.last_native_error);
 STORE(&tw->state,0);
}
static void live_try_encode(struct live_runtime *r) {
 if(LOAD(&r->stream.job)!=2)return;
 if(live_encode_cancel(r)){
  if(live_stream_abort_pending_job(&r->stream))
   live_log(&r->stream,now_us(),EV_MUX_ABORT,LOAD(&r->crumb_pad),0);
  return;
 }
 uint32_t audio=LOAD(&r->audio_phase);
 if(audio==1)return; /* worker is still freezing the audio window (bounded wait) */
 if(!ticket_lock(r))return;
 struct live_ticket t={0},*first=0,*hif_done=0;uint32_t floor_at_pair=LOAD(&r->press_ticket_floor);
 /* Pair with the first JPG opened after the accepted press (lowest generation),
  * not whichever finished JPG sits first in the slot table: a quick second shot
  * can finish before this transaction runs. Wait while that JPG is unfinished.
  * If a closed .HIF (NFP v0.5.0 SDR HEIF / HLG HEIF) arrives with no JPG, abort
  * job==2 immediately so Live never stalls or cross-pairs onto a later JPEG shot. */
 for(unsigned i=0;i<8;i++) {struct live_ticket *q=&r->tickets[i];
  if(!q->open || q->generation<=floor_at_pair || q->card_epoch!=LOAD(&r->card_epoch))continue;
  unsigned n=0;while(n<32 && q->path[n])n++;
  if(n>=4 && q->path[n-3]=='H' && q->path[n-2]=='I' && q->path[n-1]=='F'){
   if(q->success && q->closed && q->size)hif_done=q;
   continue;
  }
  if(n<4 || q->path[n-3]!='J' || q->path[n-2]!='P' || q->path[n-1]!='G')continue;
  if(!first || q->generation<first->generation)first=q;
 }
 int abort_non_jpg=0;
 if(first && first->success && first->closed && first->size){t=*first;first->open=0;}
 else if(!first && hif_done){hif_done->open=0;abort_non_jpg=1;}
 ticket_unlock(r);
 if(!t.open){
  if(abort_non_jpg || (!first && r->capture.press_us && now_us()-r->capture.press_us>20000000u)){
   if(live_stream_abort_pending_job(&r->stream))
    live_log(&r->stream,now_us(),EV_MUX_ABORT,abort_non_jpg?0x484946u:0x544fu,0);
  }
  return;
 }
 uint32_t pending=2;
 if(!__atomic_compare_exchange_n(&r->stream.job,&pending,3,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return;
 if(live_encode_cancel(r)){
  live_log(&r->stream,now_us(),EV_MUX_ABORT,LOAD(&r->crumb_pad),3u);
  STORE(&r->stream.job,4);
  return;
 }
 unsigned count=r->stream.frozen_count;int rc=-1;unsigned cleanup=0;
 live_transaction_workspace *work=r->stream.work;
 r->frame_encode_target=count;r->frame_encode_completed=0;r->frame_encode_started_us=0;r->frame_encode_finished_us=0;
 r->stream.mux_start_encoded=r->stream.encoded;
 live_native_file_context fc={.user=r,.cancelled=live_encode_cancel,.yield=live_encode_yield,.encode_event=live_encode_event};live_file_ops ops;live_native_file_ops(&ops,&fc);
 uint64_t mov_bytes=0;
 size_t i420_sz=(size_t)r->stream.width*r->stream.height*3u/2u;
 live_still_ticket still={.native_path=t.path,.native_size=t.size,.press_us=r->capture.press_us,.closed_and_registered=1,.has_jpeg=1,.volume_generation=t.card_epoch,
  .preview_frame=live_stream_preview_frame,.preview_ctx=&r->stream,.preview_scratch=r->stream.preview_scratch,
  .orient_out=&r->stream.preview_orient,.mov_bytes_out=&mov_bytes,
  .io_buf=(r->stream.i420 && i420_sz>=524288u)?r->stream.i420:0,
  .io_bytes=(r->stream.i420 && i420_sz>=524288u)?524288u:0u};
 r->stream.preview_orient=1;
 crumb(r,now_us(),31u /* v55 shutter hand-off */,LOAD(&r->shutter.phase)|(r->shutter.captured&255u)<<8|(r->shutter.played&255u)<<16|(r->shutter.completed&255u)<<24,
  (uint64_t)(r->shutter.cancelled&255u)|(uint64_t)(r->shutter.errors&255u)<<8|(uint64_t)(live_shutter_mute_get()&1u)<<16|(uint64_t)*(volatile uint8_t *)(uintptr_t)UINT64_C(0xa03bc7008)<<24|(uint64_t)(*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa02d4cb48)&255u)<<32|(uint64_t)(r->audio_z8.phase&255u)<<40);
 crumb(r,now_us(),30u /* v28 EV_AUDIO */,(uint32_t)audio|(LOAD(&r->audio_phase)<<8)|((r->audio_error&0xffu)<<16),r->audio_starts);
 if(audio==2){still.audio_get=live_audio_ticket_get;still.audio_trak=live_audio_ticket_trak;
  still.audio_ctx=r;still.audio_frames=r->audio_info.frames;r->audio_tracks++;}
 /* No synthetic PCM: a failed recording remains video-only and EV_AUDIO retains its status. */
 const char hex[]="0123456789ABCDEF";uint64_t seed=r->capture.press_us^((uint64_t)t.file_id<<32)^t.generation;
 for(unsigned i=0,j=0;i<36;i++){if(i==8||i==13||i==18||i==23)still.uuid[i]='-';else{seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;still.uuid[i]=hex[(seed>>(j++&15))&15];}}
 for(unsigned i=0;i<32;i++)r->debug_ticket_path[i]=t.path[i];
 char checkjpg[128],checkmov[128];r->debug_path_result=(uint32_t)(-live_paths(t.path,checkjpg,checkmov));
 {
  /* v68/v70 telemetry crumbs (EV_DIAG1=32, EV_DIAG2=33, EV_DIAG3=34) for real-camera LIVELOG.BIN */
  const struct live_diag *D=&r->stream.diag;
  uint64_t avg_c=D->n_convert?D->sum_convert_us/D->n_convert:0;
  uint64_t max_c=D->max_convert_us>65535u?65535u:D->max_convert_us;
  uint64_t avg_e=r->stream.encode_avg_us>65535u?65535u:r->stream.encode_avg_us;
  uint64_t dt=now_us();
  crumb(r,dt,32u,(uint32_t)(D->ok&0xffffu)|((uint32_t)(r->stream.cfr_repeats&0xffffu)<<16),
   (D->seq_changed&0xffffu)|((D->accepted_late&0xffffu)<<16)|((uint64_t)(r->stream.fr_dropped&0xffffu)<<32)|((r->stream.dup_skipped&0xffffu)<<48));
  crumb(r,dt,33u,(D->nbufs&0xffu)|((uint32_t)(D->max_frames_during_copy&0xffu)<<8)|
   ((uint32_t)((D->valid1_fail+D->valid2_fail)&0xffu)<<16)|((uint32_t)((D->stale+D->lease_fail)&0xffu)<<24),
   (avg_c&0xffffu)|(max_c<<16)|(avg_e<<32)|((r->stream.cached_lv_fail&0xffu)<<48)|((uint64_t)(r->review_deferrals&0xffu)<<56));
  uint16_t avg_in=0,avg_wait=0,avg_out=0,avg_parse=0;
  uint32_t direct_cnt=0,retrig_cnt=0;
  live_encoder_diag_timings(&r->stream.encoder,&avg_in,&avg_wait,&avg_out,&avg_parse,&direct_cnt,&retrig_cnt);
  crumb(r,dt,34u,(uint32_t)avg_in|((uint32_t)avg_wait<<16),
   (uint64_t)avg_out|((uint64_t)avg_parse<<16)|((uint64_t)(direct_cnt&0xffffu)<<32)|((uint64_t)(retrig_cnt&0xffffu)<<48));
 }
 live_log(&r->stream,now_us(),EV_MUX_BEGIN,count,t.generation);
 r->encode_started_us=now_us();rc=live_write_pair_aus(work,&ops,&still,r->stream.frozen,count,r->stream.width,r->stream.height,r->stream.last_duration_us,&cleanup);r->encode_finished_us=now_us();
 r->debug_fs_error=fc.last_native_error;r->debug_dir_stage=fc.dir_stage;
 for(unsigned i=0;i<128;i++)r->debug_dir_path[i]=fc.dir_path[i];
 r->stream.cleanup_failed=cleanup;r->stream.mux_end_encoded=r->stream.encoded;
 r->transaction_error=(uint32_t)(-rc);if(!rc)r->transaction_count++;
 if(!rc && !live_encode_cancel(r) && !LOAD(&r->crumb_pad) && mov_bytes && (t.path[0]=='A'||t.path[0]=='B') && t.path[1]==':'){
  /* Backup / JPEG+JPEG roles: the same still may also be written to the other card. */
  struct live_twin *tw=LIVE_TWIN;
  if(tw->magic!=LIVE_TWIN_MAGIC){for(unsigned i=0;i<sizeof(*tw);i++)((volatile uint8_t *)tw)[i]=0;tw->magic=LIVE_TWIN_MAGIC;}
  for(unsigned i=0;i<32;i++)tw->source[i]=t.path[i];
  for(unsigned i=0;i<36;i++)tw->uuid[i]=still.uuid[i];
  tw->epoch=t.card_epoch;tw->floor=floor_at_pair;tw->mov_bytes=mov_bytes;
  tw->deadline_us=now_us()+20000000u;STORE(&tw->state,1);
 }
 if(!rc && !live_encode_cancel(r) && r->stream.dcim_copy && r->stream.frozen_bytes){
  r->stream.dcim_result=(uint32_t)live_dcim_copy(&ops,t.path,checkmov,r->stream.frozen_bytes,524288u);
  if(!r->stream.dcim_result)r->stream.dcim_copies++;live_log(&r->stream,now_us(),EV_DCIM,r->stream.dcim_result,(uint64_t)fc.last_native_error);}
 live_log(&r->stream,now_us(),EV_MUX_END,(uint32_t)(-rc),(uint64_t)fc.last_native_error|(uint64_t)fc.dir_stage<<32);
 STORE(&r->stream.job,4);
}
