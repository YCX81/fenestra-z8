/* QEMU-only best-effort integration. Snapshot group A is an assumption on
 * hardware and an emulator-defined observable in this private QEMU build. */
#include "runtime.h"
#include "ui/live_ui.h"
#include "capture/native_capture.h"
#include "codec/live_transaction.h"
#include "codec/native_files.h"
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
}
uint32_t live_status(void) {
 struct live_runtime *r=state();if(!r)return LIVE_ERROR;
 uint32_t p=LOAD(&r->capture.phase),v=LOAD(&r->requested)?LIVE_REQUESTED:0;
 if(p==LA_READY)v|=LIVE_READY;
 if(p==LA_POST || p==LA_FROZEN || p==LA_ENCODING)v|=LIVE_BUSY;
 if(LOAD(&r->task_error) || p==LA_FAILED)v|=LIVE_ERROR;
 return v;
}
void live_runtime_event(uint32_t event) {
 struct live_runtime *r=state();if(!r)return;
 if(event==7){STORE(&r->shutdown,1);live_requested_set(0);}
 if(event==24){live_requested_set(0);__atomic_add_fetch(&r->card_epoch,1,__ATOMIC_RELAXED);la_cancel(&r->capture);}
}
void live_irq_notify(uint32_t event,uint64_t status) {
 struct live_runtime *r=state();if(r)la_irq_signal(&r->capture,event,status,now_us());
}
void live_press_notify(void) {
 struct live_runtime *r=state();if(!r)return;
 __atomic_add_fetch(&r->full_presses,1,__ATOMIC_RELAXED);
 live_log(&r->stream,now_us(),EV_PRESS,LOAD(&r->capture.phase),LOAD(&r->stream.job));
 if(LOAD(&r->stream.job) || LOAD(&r->capture.phase)!=LA_READY){r->stream.ignored_presses++;return;}
 if(la_full_press(&r->capture,now_us()))
  STORE(&r->press_ticket_floor,LOAD(&r->ticket_generation));
}
static void live_try_encode(struct live_runtime *r);
static void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b);
/* Stock Wave5 bring-up hook (0x402f2d58), caller task: give Wave5 back to movie/playback. */
void live_w5_bringup_notify(uint32_t client) {
 struct live_runtime *r=state();if(r){crumb(r,now_us(),EV_W5,client,LOAD(&r->stream.job));live_encoder_preempt(&r->stream.encoder,client);}
}
/* DMA callback context (audio-driver task): registry read only. */
struct live_audio_z8 *live_audio_z8_context(void) {
 struct live_runtime *r=state();return r?&r->audio_z8:0;
}
/* Worker task only. Audio follows the video clip: mark at the accepted press,
 * freeze the same [clip_start, clip_end) once video froze, release after the mux.
 * Any audio failure leaves the clip video-only. */
static void live_audio_step(struct live_runtime *r,int want,uint64_t now) {
 struct live_audio *a=&r->audio;
 uint32_t job=LOAD(&r->stream.job),phase=LOAD(&r->audio_phase);
 if(!want || la_cancelled(&r->capture)) {
  live_audio_cancel(a);
  if(phase==2 && (job==2||job==3))return; /* the mux may still be reading */
  if(a->active)live_audio_stop(a);
  STORE(&r->audio_phase,0);return;
 }
 if(!a->active) {
  if(now<r->audio_retry_us)return;
  int e=live_audio_start(a);
  if(e){r->audio_error=(uint32_t)-e;r->audio_retry_us=now+2000000u;return;}
  r->audio_starts++;
 }
 int e=live_audio_z8_poll(a,&r->audio_z8);
 if(e && e!=LAUDIO_GAP)r->audio_error=(uint32_t)-e;
 if(job==1 && !phase)
  STORE(&r->audio_phase,live_audio_mark(a,r->stream.clip_press)?3u:1u);
 else if(job==2 && phase==1) {
  struct live_audio_window w={r->stream.clip_start,r->stream.clip_end};
  int f=live_audio_freeze(a,w);
  struct live_audio_info info;uint32_t got=0;
  if(!f && !live_audio_get(a,0,0,0,&got,&info) && info.frames){r->audio_info=info;STORE(&r->audio_phase,2);}
  else if(f!=LAUDIO_GAP || now>r->stream.clip_end+1500000u){r->audio_skipped++;STORE(&r->audio_phase,3);}
 }
 else if(job==2 && !phase)STORE(&r->audio_phase,3);
 else if((job==0||job==4) && phase){live_audio_thaw(a);STORE(&r->audio_phase,0);}
}
static int live_audio_ticket_get(void *ctx,uint32_t off,int16_t *out,uint32_t cap,uint32_t *got) {
 struct live_runtime *r=ctx;struct live_audio_info info;
 int e=live_audio_get(&r->audio,off,out,cap,got,&info);
 if(!e&&got)for(uint32_t i=0;i<*got;i++){int32_t s=(int32_t)out[i]*4;out[i]=(int16_t)(s>32767?32767:s<-32768?-32768:s);} /* v28: +12 dB */
 return e;
}
static size_t live_audio_ticket_trak(void *ctx,uint8_t *out,size_t cap,uint64_t at) {
 struct live_runtime *r=ctx;
 return live_audio_mov_trak(out,cap,&r->audio_info,3,1000000,at);
}
#define LIVE_FLAG_PATH "A:\\FENESTRA\\boot-z8.flag"
#define LIVE_LOG_PATH "A:\\LIVEDATA\\LIVELOG.BIN"
#define LIVE_PREV_PATH "A:\\LIVEDATA\\LIVEPREV.BIN"
static void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b){
 live_log(&r->stream,t,type,a,b);uint32_t i=__atomic_fetch_add(&r->crumb_head,1,__ATOMIC_RELAXED)%32u;r->crumbs[i]=(struct live_ev){t,type,a,b};}
void live_runtime_ui_event(uint32_t event){struct live_runtime *r=state();if(r)crumb(r,now_us(),EV_UI,event,0);}
/* UI task, event 7 with USB attached: the worker parks the boot flag; wait for it (bounded). */
int live_runtime_flag_park(int park){
 struct live_runtime *r=state();if(!r||!LOAD(&r->task_started))return -1;
 STORE(&r->flag_req,park?1u:3u);
 for(unsigned i=0;i<300 && (LOAD(&r->flag_req)==1||LOAD(&r->flag_req)==3);i++)FN(0x40187b00,int32_t (*)(uint32_t))(1);
 return LOAD(&r->flag_req)==2||LOAD(&r->flag_req)==0?0:-1;
}
static void log_step(struct live_runtime *r,uint64_t now,int force);
static void flag_step(struct live_runtime *r,uint64_t now){
 uint32_t q=LOAD(&r->flag_req);
 if(q==3){log_step(r,now,1);STORE(&r->flag_req,0);return;}
 if(q!=1 && !(q==2 && r->flag_n && now-r->flag_t>10000000u))return;
 live_native_file_context fc={.user=r};live_file_ops f;live_native_file_ops(&f,&fc);int fd=-1;size_t got=0;
 if(q==1){
  r->flag_n=0;
  if(!f.open_read(f.opaque,LIVE_FLAG_PATH,&fd)){
   if(f.read(f.opaque,fd,r->flag_buf,sizeof r->flag_buf,&got))got=0;f.close(f.opaque,fd);
   if(got && got<sizeof r->flag_buf && !f.remove_owned(f.opaque,LIVE_FLAG_PATH)){r->flag_n=(uint32_t)got;r->flag_parks++;}
   else r->flag_err++;
  }
  r->flag_t=now;crumb(r,now,EV_FLAG,1,r->flag_n);log_step(r,now,1);STORE(&r->flag_req,2);return;
 }
 /* Still running 10 s after event 7: a standby, not a power-off. Put the same flag back. */
 uint32_t ok=0;size_t w=0;
 if(!f.create_exclusive(f.opaque,LIVE_FLAG_PATH,&fd)){ok=!f.write(f.opaque,fd,r->flag_buf,r->flag_n,&w)&&w==r->flag_n;if(f.close(f.opaque,fd))ok=0;}
 if(ok)r->flag_restores++;else r->flag_err++;
 crumb(r,now,EV_FLAG,ok?2:3,r->flag_n);r->flag_n=0;STORE(&r->flag_req,0);
}
/* LIVELOG.BIN: 32-byte header {'LLG2', log head, now_us, parks, restores, flag errors, saves, crumb head}
 * + 32 crumbs (important events) + the 128-event ring, all struct live_ev (24 B). */
static void log_step(struct live_runtime *r,uint64_t now,int force){
 uint32_t h=LOAD(&r->stream.log_head),ch=LOAD(&r->crumb_head);
 if(!force && (ch==r->log_saved_head || now<20000000u || now-r->log_saved_t<5000000u || LOAD(&r->stream.job) || LOAD(&r->flag_req)==1 || r->last_photo!=2))return;
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
 uint32_t hd[8]={0x32474c4cu,h,(uint32_t)now,(uint32_t)(now>>32),r->flag_parks,r->flag_restores,r->flag_err,LOAD(&r->crumb_head)};
 int ok=0;
 if(!f.create_exclusive(f.opaque,LIVE_LOG_PATH,&out)){
  ok=!f.write(f.opaque,out,hd,sizeof hd,&w)&&w==sizeof hd&&!f.write(f.opaque,out,r->crumbs,sizeof r->crumbs,&w)&&w==sizeof r->crumbs
     &&!f.write(f.opaque,out,r->stream.log,sizeof r->stream.log,&w)&&w==sizeof r->stream.log;
  if(f.close(f.opaque,out))ok=0;
 }
 if(ok){r->log_saves++;r->log_saved_head=ch;}else r->log_err++;
}
static const uint32_t lv_reg_addr[162]={0x15421000u,0x15421004u,0x15421008u,0x1542100cu,0x15421010u,0x15421014u,0x15421018u,0x15421020u,0x15421024u,0x15421028u,0x1542102cu,0x15421038u,0x1542103cu,0x15421040u,0x15421048u,0x1542104cu,0x15421050u,0x15421054u,0x15422000u,0x1542200cu,0x15422010u,0x15422014u,0x15422018u,0x15422020u,0x15422024u,0x15422028u,0x1542202cu,0x15422038u,0x1542203cu,0x15422040u,0x15422048u,0x15422060u,0x1542206cu,0x15422070u,0x15422100u,0x1542210cu,0x15422110u,0x15422114u,0x15422118u,0x15422120u,0x15422124u,0x15422128u,0x1542212cu,0x15422138u,0x1542213cu,0x15422140u,0x15422148u,0x15422160u,0x1542216cu,0x15422170u,0x15422200u,0x1542220cu,0x15422210u,0x15422214u,0x15422218u,0x15422220u,0x15422224u,0x15422228u,0x1542222cu,0x15422238u,0x1542223cu,0x15422240u,0x15422248u,0x15422260u,0x1542226cu,0x15422270u,0x15423000u,0x15423004u,0x15423008u,0x1542300cu,0x15423010u,0x15423014u,0x15423018u,0x1542301cu,0x15423020u,0x15423024u,0x15423028u,0x1542302cu,0x15423030u,0x15423034u,0x15423038u,0x15423100u,0x15423104u,0x15423108u,0x1542310cu,0x15423110u,0x15423114u,0x15423118u,0x1542311cu,0x15423120u,0x15423124u,0x15423128u,0x1542312cu,0x15423130u,0x15423134u,0x15423138u,0x15423200u,0x15423204u,0x15423208u,0x1542320cu,0x15423210u,0x15423214u,0x15423218u,0x1542321cu,0x15423220u,0x15423224u,0x15423228u,0x1542322cu,0x15423230u,0x15423234u,0x15423238u,0x15425700u,0x15425704u,0x15425708u,0x1542570cu,0x15425710u,0x15425714u,0x15425720u,0x15425724u,0x15425730u,0x15425734u,0x15425738u,0x1542573cu,0x15425740u,0x15425744u,0x15425748u,0x1542574cu,0x15425780u,0x15425f00u,0x15425f04u,0x15425f08u,0x15425f0cu,0x15425f10u,0x15425f14u,0x15425f20u,0x15425f24u,0x15425f30u,0x15425f34u,0x15425f38u,0x15425f3cu,0x15425f40u,0x15425f44u,0x15425f48u,0x15425f4cu,0x15425f80u,0x15426700u,0x15426704u,0x15426708u,0x1542670cu,0x15426710u,0x15426714u,0x15426720u,0x15426724u,0x15426730u,0x15426734u,0x15426738u,0x1542673cu,0x15426740u,0x15426744u,0x15426748u,0x1542674cu,0x15426780u};
static const uint32_t ch2_addr[12]={0x15423208u,0x1542320cu,0x15423210u,0x15423214u,0x1542322cu,0x15423230u,0x1542220cu,0x15422238u,0x1542223cu,0x15426720u,0x15423200u,0x15423204u},ch2_val[12]={0x0u,0x0u,0x4000u,0x4000u,0x500u,0x354u,0x1000000u,0x500u,0x500u,0x222u,0x0u,0x1u};
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
/* v19: kick output 2 as the stock code restarts a single output: B2 +0x00 1 -> 0, then B2 +0x04 = 1 */
static void ch2_kick(void){REG32(0x15422200u)=1u;__asm__ volatile("dsb sy":::"memory");REG32(0x15422200u)=0u;
 __asm__ volatile("dsb sy":::"memory");REG32(0x15422204u)=1u;__asm__ volatile("dsb sy":::"memory");}
/* v19: channel 2 at 1:1 on request (photo mode only); 1 = stock mode 4, 2 = mode 1 variant; 0 = restore. */
static void ch2_step(struct live_runtime *r,int photo){
 uint32_t q=LOAD(&r->ch2_req),s=r->ch2_state;
 if((q==1u||q==2u)&&photo){
  if(s==q&&REG32(0x1542322c)==1280u)return;
  if(s==1u||s==2u)r->ch2_reapplies++;else for(unsigned i=0;i<12u;i++)r->ch2_saved[i]=REG32(ch2_addr[i]);
  unsigned n=q==2u?12u:10u;for(unsigned i=0;i<n;i++)REG32(ch2_addr[i])=ch2_val[i];
  __asm__ volatile("dsb sy":::"memory");ch2_kick();r->ch2_state=q;r->ch2_applies++;return;}
 if(q==0u&&(s==1u||s==2u)){for(unsigned i=12u;i-->0;)REG32(ch2_addr[i])=r->ch2_saved[i];
  __asm__ volatile("dsb sy":::"memory");ch2_kick();r->ch2_state=0u;r->ch2_restores++;}
}
/* v23: GUI view and playback-module changes as crumbs (all reads are RAM, range-checked). */
static int v23_ram(uint64_t a,uint64_t n){return a>=UINT64_C(0xa00000000)&&a+n<=UINT64_C(0xa40000000)&&!(a&3);}
static void vlog(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b){uint32_t i=r->vhead++%64u;r->vring[i]=(struct live_ev){t,type,a,b};}
static void view_step(struct live_runtime *r,uint64_t now){
 uint64_t v=*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa032280c8);uint32_t is=0xffffffffu;
 if(v23_ram(v,0x110))is=(*(volatile uint32_t *)(uintptr_t)(v+0x54))<<8|((*(volatile uint32_t *)(uintptr_t)(v+0xd0))&0xffu);
 if(v!=r->last_view||is!=r->last_view_is){r->last_view=v;r->last_view_is=is;vlog(r,now,EV_VIEW,is,v);}
 uint64_t a0=*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa032b50d8);if(!v23_ram(a0,8))return;
 uint64_t core=*(volatile uint64_t *)(uintptr_t)(a0+663*96+80);if(!v23_ram(core,2536))return;
 uint64_t reg=*(volatile uint64_t *)(uintptr_t)(core+2528);if(!v23_ram(reg,24))return;
 uint64_t pb=*(volatile uint64_t *)(uintptr_t)(reg+16);if(!v23_ram(pb,8240))return;
 if(*(volatile uint32_t *)(uintptr_t)pb!=0x50425549u)return;
 #define PB32(o) (*(volatile uint32_t *)(uintptr_t)(pb+(o)))
 uint32_t a=(PB32(76)&0xffu)|(PB32(8208)&0xffu)<<8|(PB32(8212)&0xffu)<<16|(PB32(8228)&0xffu)<<24;
 uint64_t b=(uint64_t)PB32(8216)|(uint64_t)PB32(8236)<<32;
 if(a!=r->last_pb_a||b!=r->last_pb_b){r->last_pb_a=a;r->last_pb_b=b;vlog(r,now,EV_PB,a,b);}
 #undef PB32
}
static void mux_worker(void *arg) {
 struct live_runtime *r=arg;STORE(&r->stream.mux_started,1);
 for(;;){live_try_encode(r);FN(0x40187b00,int32_t (*)(uint32_t))(5);}
}
static void worker(void *arg) {
 struct live_runtime *r=arg;STORE(&r->task_started,1);
 for(;;) {
  struct live_diag *D=&r->stream.diag;uint64_t w0=now_us();
  if(D->loop_t0){D->last_loop_us=w0-D->loop_t0;if(D->last_loop_us>D->max_loop_us)D->max_loop_us=D->last_loop_us;}
  D->loop_t0=w0;D->loops++;
  live_ui_poll();r->poll_count++;
  uint64_t w1=now_us();D->last_ui_us=w1-w0;if(D->last_ui_us>D->max_ui_us)D->max_ui_us=D->last_ui_us;
  /* PHOTO is the only implemented mode gate; other release modes remain
   * unvalidated. Keep the worker outside the native shooting path. */
  int photo=*(volatile uint32_t *)(uintptr_t)UINT64_C(0xa03bd062c)==0;
  if((uint32_t)photo+1u!=r->last_photo){r->last_photo=(uint32_t)photo+1u;crumb(r,w1,EV_MODE,(uint32_t)photo,0);}
  uint64_t w2=now_us();
  live_stream_step(&r->capture,&r->stream,now_us(),photo && !LOAD(&r->shutdown),now_us);
  uint64_t w3=now_us();D->last_step_us=w3-w2;if(D->last_step_us>D->max_step_us)D->max_step_us=D->last_step_us;
  uint64_t t=now_us(),lv=LOAD(&r->capture.irq_timestamp);
  int lv_on=lv && t>=lv && t-lv<UINT64_C(1500000);
  live_audio_step(r,r->stream.tune.audio && photo && !LOAD(&r->shutdown) && LOAD(&r->requested) && (lv_on || LOAD(&r->stream.job)),t);
  uint64_t w4=now_us();D->last_audio_us=w4-t;if(D->last_audio_us>D->max_audio_us)D->max_audio_us=D->last_audio_us;
  flag_step(r,now_us());log_step(r,now_us(),0);ch2_step(r,photo);view_step(r,now_us());
  /* v32: no close at the mode switch; the bring-up hook hands Wave5 back */
  if(now_us()-r->lv_geo_t>1000000u){r->lv_geo_t=now_us();
   for(unsigned k=0;k<4;k++){struct la_native_geometry g={0};uint32_t ok=(uint32_t)la_native_geometry_k(&g,k);
    r->lv_geo[k][0]=g.width;r->lv_geo[k][1]=g.height;r->lv_geo[k][2]=g.y_stride;r->lv_geo[k][3]=g.uv_stride;r->lv_geo[k][4]=g.nv12;r->lv_geo[k][5]=ok;}
   if(r->lv_regs_on==1u){for(unsigned i=0;i<162u;i++)r->lv_regs[i]=*(volatile uint32_t *)(uintptr_t)lv_reg_addr[i];
   r->lv_regs_n++;}}
  FN(0x40187b00,int32_t (*)(uint32_t))(5);
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
 if(!la_native_cpu2_start(mt,mux_worker,r,
 (void *)(((uintptr_t)r->stream.mux_stack_raw+15)&~(uintptr_t)15),65536,31)){
  STORE(&r->task_error,mt->native_error?(uint32_t)mt->native_error:2u);return;}

 r->worker_stack_raw=FN(0x40d2c408,void *(*)(uint32_t,uint32_t))(65536+16,0);
 if(!r->worker_stack_raw){STORE(&r->task_error,1);return;}
 struct la_native_task *task=(struct la_native_task *)r->task_scope;
 if(!la_native_cpu2_start(task,worker,r,
   (void *)(((uintptr_t)r->worker_stack_raw+15)&~(uintptr_t)15),65536,30))
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
  for(unsigned i=0;i<8;i++)if(r->tickets[i].open && r->tickets[i].fd==fd)r->tickets[i].size=size;
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
static int live_encode_cancel(void *p){struct live_runtime *r=p;return !LOAD(&r->capture.requested)||LOAD(&r->shutdown);}
static void live_encode_yield(void *p){(void)p;la_native_yield();}
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
 for(;;){size_t got=0,w=0;if(f->read(f->opaque,in,buf,cap,&got)){rc=5;break;}if(!got)break;
  if(f->write(f->opaque,out,buf,got,&w)||w!=got){rc=6;break;}if(got<cap)break;if(f->yield)f->yield(f->opaque);}
 f->close(f->opaque,in);if(f->close(f->opaque,out)&&!rc)rc=7;if(rc)f->remove_owned(f->opaque,dst);return rc;}
static void live_try_encode(struct live_runtime *r) {
 if(LOAD(&r->stream.job)!=2)return;
 if(!LOAD(&r->capture.requested)||LOAD(&r->shutdown)){STORE(&r->stream.job,4);live_log(&r->stream,now_us(),EV_MUX_ABORT,0,0);return;}
 uint32_t audio=LOAD(&r->audio_phase);
 if(audio==1)return; /* worker is still freezing the audio window (bounded wait) */
 if(!ticket_lock(r))return;
 struct live_ticket t={0},*first=0;
 /* Pair with the first JPG opened after the accepted press (lowest generation),
  * not whichever finished JPG sits first in the slot table: a quick second shot
  * can finish before this transaction runs. Wait while that JPG is unfinished. */
 for(unsigned i=0;i<8;i++) {struct live_ticket *q=&r->tickets[i];
  if(!q->open || q->generation<=LOAD(&r->press_ticket_floor) || q->card_epoch!=LOAD(&r->card_epoch))continue;
  unsigned n=0;while(n<32 && q->path[n])n++;
  if(n<4 || q->path[n-3]!='J' || q->path[n-2]!='P' || q->path[n-1]!='G')continue;
  if(!first || q->generation<first->generation)first=q;
 }
 if(first && first->success && first->closed && first->size){t=*first;first->open=0;}
 ticket_unlock(r);if(!t.open)return;
 uint32_t pending=2;
 if(!__atomic_compare_exchange_n(&r->stream.job,&pending,3,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return;
 unsigned count=r->stream.frozen_count;int rc=-1;unsigned cleanup=0;
 live_transaction_workspace *work=r->stream.work;
 r->frame_encode_target=count;r->frame_encode_completed=0;r->frame_encode_started_us=0;r->frame_encode_finished_us=0;
 r->stream.mux_start_encoded=r->stream.encoded;
 live_native_file_context fc={.user=r,.cancelled=live_encode_cancel,.yield=live_encode_yield,.encode_event=live_encode_event};live_file_ops ops;live_native_file_ops(&ops,&fc);
 live_still_ticket still={.native_path=t.path,.native_size=t.size,.press_us=r->capture.press_us,.closed_and_registered=1,.has_jpeg=1,.volume_generation=t.card_epoch,
  .preview_frame=live_stream_preview_frame,.preview_ctx=&r->stream,.preview_scratch=r->stream.preview_scratch,
  .orient_out=&r->stream.preview_orient};
 r->stream.preview_orient=1;
 crumb(r,now_us(),30u /* v28 EV_AUDIO */,(uint32_t)audio|(LOAD(&r->audio_phase)<<8)|((r->audio_error&0xffu)<<16),r->audio_starts);
 if(audio==2){still.audio_get=live_audio_ticket_get;still.audio_trak=live_audio_ticket_trak;
  still.audio_ctx=r;still.audio_frames=r->audio_info.frames;r->audio_tracks++;}
 const char hex[]="0123456789ABCDEF";uint64_t seed=r->capture.press_us^((uint64_t)t.file_id<<32)^t.generation;
 for(unsigned i=0,j=0;i<36;i++){if(i==8||i==13||i==18||i==23)still.uuid[i]='-';else{seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;still.uuid[i]=hex[(seed>>(j++&15))&15];}}
 for(unsigned i=0;i<32;i++)r->debug_ticket_path[i]=t.path[i];
 char checkjpg[128],checkmov[128];r->debug_path_result=(uint32_t)(-live_paths(t.path,checkjpg,checkmov));
 live_log(&r->stream,now_us(),EV_MUX_BEGIN,count,t.generation);
 r->encode_started_us=now_us();rc=live_write_pair_aus(work,&ops,&still,r->stream.frozen,count,r->stream.width,r->stream.height,r->stream.last_duration_us,&cleanup);r->encode_finished_us=now_us();
 r->debug_fs_error=fc.last_native_error;r->debug_dir_stage=fc.dir_stage;
 for(unsigned i=0;i<128;i++)r->debug_dir_path[i]=fc.dir_path[i];
 r->stream.cleanup_failed=cleanup;r->stream.mux_end_encoded=r->stream.encoded;
 r->transaction_error=(uint32_t)(-rc);if(!rc)r->transaction_count++;
 if(!rc && r->stream.dcim_copy && r->stream.frozen_bytes){
  r->stream.dcim_result=(uint32_t)live_dcim_copy(&ops,t.path,checkmov,r->stream.frozen_bytes,LIVE_BITSTREAM_BYTES);
  if(!r->stream.dcim_result)r->stream.dcim_copies++;live_log(&r->stream,now_us(),EV_DCIM,r->stream.dcim_result,(uint64_t)fc.last_native_error);}
 live_log(&r->stream,now_us(),EV_MUX_END,(uint32_t)(-rc),(uint64_t)fc.last_native_error|(uint64_t)fc.dir_stage<<32);
 STORE(&r->stream.job,4);
}
