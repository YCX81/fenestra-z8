#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#include "runtime_selected.inc"
static struct live_runtime r;
static struct live_audio_z8 *z=&r.audio_z8;
static struct live_audio *a=&r.audio;
static uint64_t clock_us=10000000,ring[3]={0x12340000,0,5};
static uint32_t service=1,allocs,frees,nq,read_n,write_n,fail_write;
static struct laz8_msg *queue[64];
static int16_t pcm[LAZ8_BLOCK_FRAMES*2];
struct live_audio_z8 *live_audio_z8_context(void){return z;}
int la_cancelled(const struct la_capture*c){return !c->requested||c->cancel_epoch!=c->worker_epoch;}
static int post(struct laz8_msg*m){assert(nq<64);queue[nq++]=m;return 1;}
static int read_(uint32_t reg,uint16_t*v){assert(reg==0x15||reg==0x16);read_n++;*v=0x100;return 0;}
static int write_(uint32_t reg,uint32_t v){assert(reg==0x15||reg==0x16);(void)v;write_n++;return fail_write;}
static uint64_t clock_(void){return clock_us;}
static void sleep_(uint32_t ms){(void)ms;assert(!"worker must not wait");}
int live_audio_native_alloc(void*x,uint32_t n,void**d,uint8_t**p,uint32_t*actual){(void)x;*d=calloc(1,n);assert(*d);*p=*d;*actual=n;allocs++;return 0;}
void live_audio_native_free(void*x,void*d){(void)x;assert(z->phase==LAZ8_IDLE);free(d);frees++;}
uint64_t live_audio_clock_us(void){return clock_us;}
static void consume(void){
 for(unsigned i=0;i<nq;i++){
  struct laz8_msg*m=queue[i];uint8_t*p=m->payload;
  if(m->id==0x42040104){p[0x20]=1;service=4;}
  if(m->id==0x42040106){p[6]=1;service=5;}
  if(m->id==0x42040108){p[0]=1;service=1;}
  if(m->param)((void(*)(uint32_t,void*))(uintptr_t)m->param)(m->id,(void*)(uintptr_t)m->tail);
 }nq=0;
}
static void reset(void){
 if(a->active){live_audio_stop(a);consume();assert(!live_audio_stop(a));}
 assert(allocs==frees);memset(&r,0,sizeof r);clock_us+=10000000;service=1;read_n=write_n=fail_write=nq=0;
 live_audio_z8_init(z,0);z->native=(struct laz8_native){post,read_,write_,sleep_,&service,ring,live_audio_native_alloc,live_audio_native_free,clock_};
 struct live_audio_ops o;live_audio_z8_ops(z,&o);live_audio_init(a,&o);
 r.requested=r.capture.requested=1;r.stream.tune.audio=1;r.stream.tune.window_us=1500000;
 for(unsigned i=0;i<LAZ8_BLOCK_FRAMES*2;i++)pcm[i]=(int16_t)(300+i%400);
}
static void block(uint64_t stamp){clock_us=stamp;live_audio_z8_block(pcm,sizeof pcm);}
static void video_idr(uint64_t stamp){r.stream.ring.count=1;r.stream.ring.entries[0]=(struct au_entry){stamp,0,100,1};}
int main(void){
 reset();uint64_t begin=clock_us;live_audio_step(&r,1,clock_us);live_audio_ready_step(&r,clock_us);
 assert(a->active && r.audio_warm && !z->unmuted && nq==6);
 // No native completion and no ADC: remains Preparing, no fake silent readiness.
 clock_us+=3000000;video_idr(clock_us-2000000);live_audio_step(&r,1,clock_us);live_audio_ready_step(&r,clock_us);
 assert(r.audio_warm && !write_n && !a->count);
 consume();assert(z->go[7] && !write_n);live_audio_step(&r,1,clock_us);
 assert(z->unmuted && write_n==2 && !z->head && !a->count);
 puts("PASS GO completion un-mutes before first PCM; empty open backend never Ready");
 uint64_t unmute=clock_us;
 block(unmute+LAZ8_BLOCK_US-1);live_audio_step(&r,1,clock_us);assert(!a->count);
 block(unmute+LAZ8_BLOCK_US*2);live_audio_step(&r,1,clock_us);assert(a->count==LAZ8_BLOCK_FRAMES);
 assert(a->first_index==(unmute+LAZ8_BLOCK_US)*48/1000);
 puts("PASS block crossing un-mute rejected; next complete block becomes history");
 for(unsigned i=3;i<=5;i++){block(unmute+i*LAZ8_BLOCK_US);live_audio_step(&r,1,clock_us);}
 video_idr(unmute+LAZ8_BLOCK_US-25);live_audio_ready_step(&r,clock_us);assert(r.audio_warm);
 video_idr(unmute+LAZ8_BLOCK_US);live_audio_ready_step(&r,clock_us);assert(!r.audio_warm);
 clock_us+=LAZ8_BLOCK_US*2+1;live_audio_ready_step(&r,clock_us);assert(r.audio_warm);
 puts("PASS Ready requires actual video IDR in complete, fresh audio history");
 assert(!live_audio_mark(a,unmute+LAZ8_BLOCK_US*3));
 struct live_audio_window w={unmute,unmute+LAZ8_BLOCK_US*5};
 assert(live_audio_freeze(a,w)==LAUDIO_GAP);w.begin_us=unmute+LAZ8_BLOCK_US;assert(!live_audio_freeze(a,w));
 struct live_audio_info info;int16_t out[64];uint32_t got;
 assert(!live_audio_get(a,0,out,32,&got,&info)&&got==32&&out[0]!=0);
 // Overrun must never thaw a frozen ring while mux is reading it.
 uint64_t first=a->freeze_first;for(unsigned i=0;i<LAZ8_SLOTS+1;i++)block(clock_us+LAZ8_BLOCK_US);
 assert(live_audio_z8_poll(a,z)==LAUDIO_GAP && a->frozen && a->freeze_first==first);
 puts("PASS missing prefix rejected, complete track nonzero; overrun preserves frozen mux storage");
 reset();live_audio_step(&r,1,clock_us);consume();
 // Simulate video call that delayed audio worker for 1.5 seconds after GO.
 begin=clock_us;for(unsigned i=1;i<=3;i++)block(begin+i*LAZ8_BLOCK_US);
 live_audio_step(&r,1,clock_us);assert(z->unmuted && !a->count && z->tail==z->head);
 live_audio_ready_step(&r,clock_us);assert(r.audio_warm);
 puts("PASS delayed worker discards all pre-unmute PCM instead of accepting silent history");
 reset();live_audio_step(&r,1,clock_us);consume();fail_write=1;live_audio_step(&r,1,clock_us);
 assert(r.audio_error==1 && a->cancelled && a->active && z->phase==LAZ8_STOPPING);
 live_audio_ready_step(&r,clock_us);assert(r.audio_warm);
 fail_write=0;consume();live_audio_step(&r,1,clock_us);live_audio_ready_step(&r,clock_us);
 assert(!a->active && !r.audio_warm && r.audio_error==1);
 r.stream.tune.audio=0;live_audio_ready_step(&r,clock_us);assert(!r.audio_warm);
 puts("PASS failed codec un-mute stops and retains allocation until ACK; explicit failure/disabled fallback only");
 reset();live_audio_step(&r,1,clock_us);consume();live_audio_step(&r,1,clock_us);
 begin=clock_us;for(unsigned i=1;i<=4;i++){block(begin+i*LAZ8_BLOCK_US);live_audio_step(&r,1,clock_us);}
 // Raw sample zeros are valid quiet sound; readiness must not be an amplitude gate.
 memset(pcm,0,sizeof pcm);block(clock_us+LAZ8_BLOCK_US);live_audio_step(&r,1,clock_us);
 video_idr(begin);live_audio_ready_step(&r,clock_us);assert(!r.audio_warm);
 puts("PASS acoustic silence does not masquerade as missing PCM or prevent readiness");
 reset();live_audio_step(&r,1,clock_us);consume();live_audio_step(&r,1,clock_us);
 begin=clock_us;for(unsigned i=1;i<=4;i++){block(begin+i*LAZ8_BLOCK_US);live_audio_step(&r,1,clock_us);}
 for(unsigned i=0;i<LAZ8_SLOTS+1;i++)block(clock_us+LAZ8_BLOCK_US);
 assert(live_audio_z8_poll(a,z)==LAUDIO_GAP && !a->count);
 video_idr(begin);live_audio_ready_step(&r,clock_us);assert(r.audio_warm);
 puts("PASS callback overrun invalidates non-frozen history and returns to Preparing");
 reset();live_audio_step(&r,1,clock_us);assert(live_audio_stop(a)==LAUDIO_BUSY);
 consume();assert(z->go[7] && z->phase==LAZ8_STOPPING);
 assert(!live_audio_z8_poll(a,z) && !write_n);
 assert(!live_audio_stop(a) && !a->active);
 live_audio_step(&r,1,clock_us);assert(a->active && !z->go[7]);
 assert(!live_audio_z8_poll(a,z) && !write_n);
 puts("PASS GO ACK arriving during stop never un-mutes; completed stop/new session clears old ACK");
 for(unsigned i=0;i<5;i++) {
  reset();live_audio_step(&r,1,clock_us);consume();
  service=i==0?1:i==1?5:4;z->go[6]=i<2?0:1;
  live_audio_step(&r,1,clock_us);live_audio_ready_step(&r,clock_us);
  assert(r.audio_error==1 && a->cancelled && a->active && r.audio_warm);
  assert(z->phase==LAZ8_STOPPING && !write_n);
  uint64_t retry=r.audio_retry_us;assert(retry==clock_us+2000000u);
  consume();live_audio_step(&r,1,clock_us);live_audio_ready_step(&r,clock_us);
  assert(!a->active && !r.audio_warm);
  clock_us=retry-1;live_audio_step(&r,1,clock_us);assert(!a->active);
  clock_us=retry;live_audio_step(&r,1,clock_us);assert(a->active && r.audio_starts==2);
 }
 puts("PASS all five native GO failure results stop/ACK, explicit error fallback and 2-second retry");
 reset();puts("ALL STARTUP CASES PASS");
}
