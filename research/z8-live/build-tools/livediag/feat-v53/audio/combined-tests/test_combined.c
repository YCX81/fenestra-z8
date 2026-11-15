#define main previous_tests_not_run
#include "baseline_lifecycle.c"
#undef main
#include "runtime.h"
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#include "ready.inc"
static void sample(int16_t v){
 for(unsigned i=0;i<sizeof pcm/2;i++)((int16_t*)pcm)[i]=v;
 now+=LAZ8_BLOCK_US;live_audio_z8_block(pcm,sizeof pcm);assert(!live_audio_z8_poll(&a,&z));
}
static void pause_(void){
 assert(live_audio_z8_pause(&a,&z)==LAUDIO_BUSY);consume_all();assert(!live_audio_z8_pause(&a,&z));
 assert(a.active&&a.marked&&!a.cancelled&&z.phase==LAZ8_IDLE);
}
static void resume_(uint64_t dt){now+=dt;assert(!live_audio_z8_resume(&a,&z));consume_all();assert(!live_audio_z8_poll(&a,&z));}
int main(void){
 init();assert(!live_audio_start(&a));consume_all();assert(!live_audio_z8_poll(&a,&z));
 assert(!live_audio_set_duck(&a,(struct live_audio_duck){0,0,0,32768}));
 uint64_t t=now;for(unsigned i=0;i<20;i++)sample(100);assert(!live_audio_mark(&a,now));uint64_t press=a.press_us;
 pause_();resume_(200000);for(unsigned i=0;i<5;i++)sample(200);
 pause_();resume_(100000);for(unsigned i=0;i<29;i++)sample(300);
 assert(now==t+3000000 && a.marked && a.press_us==press);
 assert(!live_audio_freeze(&a,(struct live_audio_window){t,t+3000000}));
 int16_t*out=malloc(144000*4);uint32_t got;struct live_audio_info info;
 assert(!live_audio_get(&a,0,out,144000,&got,&info)&&got==144000);
 for(unsigned i=0;i<144000;i++){
  int v=i<48000?100:i<57600?0:i<69600?200:i<74400?0:300;
  assert(out[2*i]==v&&out[2*i+1]==v);
 }
 free(out);assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 puts("PASS two SFX pauses: exact 1s+200ms gap+250ms+100ms gap+1450ms PCM, mark and 3s window preserved");
 // Readiness must preserve a cold false-ready state while held, then recompute
 // on unmuted resume. It must not reuse pre-pause warm=0 after resume begins.
 struct live_runtime r={0};r.requested=1;r.stream.tune.audio=1;r.stream.tune.window_us=1500000;
 r.audio_warm=1;r.shutter.phase=1;live_audio_ready_step(&r,t);assert(r.audio_warm==1);
 r.audio_warm=0;live_audio_ready_step(&r,t);assert(r.audio_warm==0);
 r.shutter.phase=0;r.audio.active=1;r.audio_z8.unmuted=0;live_audio_ready_step(&r,t);assert(r.audio_warm==1);
 r.stream.ring.count=1;r.stream.ring.entries[0]=(struct au_entry){t-2000000,0,1,1};
 r.audio_z8.unmuted=1;r.audio.count=96000;r.audio.first_index=(t-2000000)*48/1000;r.audio.next_index=t*48/1000;
 live_audio_ready_step(&r,t);assert(!r.audio_warm);
 r.audio.cancelled=1;live_audio_ready_step(&r,t);assert(r.audio_warm);
 puts("PASS held sound preserves cold/warm eligibility; resume and cancelled capture re-evaluate readiness");
 // Missing initial history still cannot be manufactured by an SFX bridge.
 init();assert(!live_audio_start(&a));consume_all();assert(!live_audio_z8_poll(&a,&z));
 t=now;for(unsigned i=0;i<10;i++)sample(100);assert(!live_audio_mark(&a,now));pause_();resume_(100000);
 for(unsigned i=0;i<10;i++)sample(200);
 assert(live_audio_freeze(&a,(struct live_audio_window){t-100000,now})==LAUDIO_GAP);
 assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 puts("PASS explicit shutter bridge does not restore forbidden missing leading history");
 init();assert(!live_audio_start(&a));consume_all();assert(!live_audio_z8_poll(&a,&z));
 t=now;for(unsigned i=0;i<20;i++)sample(123);assert(!live_audio_mark(&a,now-100000));
 pause_();uint64_t old_end=now;resume_(300000);
 assert(!live_audio_freeze(&a,(struct live_audio_window){t,old_end}));
 void*fd=a.descriptor;uint64_t frozen_first=a.freeze_first;uint32_t frozen_count=a.freeze_frames;
 sample(222);assert(a.frozen && a.descriptor==fd && a.freeze_first==frozen_first && a.freeze_frames==frozen_count && !a.cancelled);
 assert(z.resume_bridge);live_audio_thaw(&a);sample(333);assert(!z.resume_bridge && a.count==LAZ8_BLOCK_FRAMES);
 assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 puts("PASS freeze after resume before first PCM neither bridges nor stops frozen mux; later thaw starts fresh history");
 // A paused allocation with a frozen reader cannot restart or be freed early.
 init();assert(!live_audio_start(&a));consume_all();assert(!live_audio_z8_poll(&a,&z));t=now;
 for(unsigned i=0;i<10;i++)sample(100);assert(!live_audio_mark(&a,now-100000));pause_();
 assert(!live_audio_freeze(&a,(struct live_audio_window){t,now}));void*keep=a.descriptor;
 assert(live_audio_z8_resume(&a,&z)==LAUDIO_STATE && a.descriptor==keep && a.frozen);
 assert(!live_audio_stop(&a));assert(allocs==frees);
 puts("PASS frozen reader rejects resume; native pause already ACKed before eventual release");
}
