#include "live_audio_z8.h"
#define FN(a,t) ((t)(uintptr_t)(a))
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define MSG_BASE 0x42040101u
#define CALLBACK_CLOSED 0x80000000u
/* Payload images read back from the camera after a working `audiodbg 1 1 0`
 * (sub 0, 16-bit), SoundTest statics 0xa038ff330..0xa038ff3c7. Bytes the stock
 * code did not write are kept as observed rather than guessed. */
static const uint8_t format_image[0x28]={0,0,0,0,5,0,0,0,0,0,0,0,5};
static const uint8_t channels_image[8]={0,1,0,1,1,0,0,0};
static const uint8_t route_image[0x10]={0,0,0,0,1,0,0,0,0,0,0,0,0,1,0,0};
static const uint8_t mode_image[0x10]={0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0};
static const uint8_t go_image[8]={0,0,0,0,1,1,1,0};
static const uint8_t stop_image[8]={1,0,0,0,0,0,0,0};

static int native_post(struct laz8_msg *m) {
 return FN(0x40c80988,int (*)(struct laz8_msg *))(m)&1;
}
static int native_codec_read(uint32_t reg,uint16_t *value) {
 return FN(0x40c7fd08,int (*)(uint32_t,uint16_t *))(reg,value);
}
static int native_codec_write(uint32_t reg,uint32_t value) {
 return FN(0x40c7f824,int (*)(uint32_t,uint32_t))(reg,value);
}
static void native_sleep(uint32_t ms) {FN(0x40187b00,int32_t (*)(uint32_t))(ms);}
static void copy(void *d,const void *s,uint32_t n) {
 uint8_t *o=d;const uint8_t *i=s;
 for(uint32_t k=0;k<n;k++)o[k]=i[k];
}
static void copy_block(void *d,const void *s) { /* block bytes are a multiple of 8 */
 uint64_t *o=d;const uint64_t *i=s;
 for(uint32_t k=0;k<LAZ8_BLOCK_BYTES/8u;k++)o[k]=i[k];
}

void live_audio_z8_init(struct live_audio_z8 *z,uint32_t adc_volume) {
 uint8_t *p=(uint8_t *)z;for(size_t i=0;i<sizeof(*z);i++)p[i]=0;
 z->native=(struct laz8_native){native_post,native_codec_read,native_codec_write,native_sleep,
  (const volatile uint32_t *)(uintptr_t)UINT64_C(0xa02d4cb48),
  (const volatile uint64_t *)(uintptr_t)UINT64_C(0xa031f8bb8),
  live_audio_native_alloc,live_audio_native_free,live_audio_clock_us};
 z->adc_volume=adc_volume?adc_volume:LAZ8_ADC_0DB;
}

/* One allocation holds the live_audio ring and the callback staging slots: the
 * native allocator may round each request to a whole class unit (~11 MiB). */
static int z8_allocate(void *opaque,uint32_t bytes,void **d,uint8_t **cpu,uint32_t *actual) {
 struct live_audio_z8 *z=opaque;
 if(bytes>UINT32_MAX-LAZ8_STAGE_BYTES)return -1;
 if(z->native.allocate(0,bytes+LAZ8_STAGE_BYTES,d,cpu,actual))return -1;
 if(!*cpu || *actual<bytes+LAZ8_STAGE_BYTES || ((uintptr_t)*cpu&7))return -1;
 z->stage=*cpu+((bytes+7u)&~7u);
 if(z->stage+LAZ8_STAGE_BYTES>*cpu+*actual)return -1;
 *actual=bytes;return 0;
}
static void z8_release(void *opaque,void *d) {
 struct live_audio_z8 *z=opaque;
 z->stage=0;z->native.release(0,d);
}

static void send(struct live_audio_z8 *z,unsigned i,uint32_t id,void *payload) {
 struct laz8_msg *m=&z->msg[i];
 uint8_t *p=(uint8_t *)m;for(size_t k=0;k<sizeof(*m);k++)p[k]=0;
 m->id=id;m->payload=payload;
}
static void restore_codec(struct live_audio_z8 *z) {
 if(!z->unmuted)return;
 for(unsigned c=0;c<2u;c++) {
  uint32_t v=z->saved_valid?(uint32_t)z->saved[c]|0x100u:0x100u; /* photo default: mute */
  if(z->native.codec_write(0x15u+c,v))z->codec_errors++;
 }
 z->unmuted=0;
}

__attribute__((noinline))
static void start_complete(uint32_t event,void *context) {
 struct live_audio_z8 *z=context;
 /* Native GO reads +0..5 and writes result +6; +7 is our ACK byte.
  * The callback only publishes completion. Codec I/O stays on our worker. */
 if(z && event==MSG_BASE+5u)STORE(&z->go[7],1u);
}

static int z8_start(void *opaque) {
 struct live_audio_z8 *z=opaque;
 if(z->phase!=LAZ8_IDLE || !z->stage)return -1;
 /* 1 = idle. Movie standby (5) or a running SoundTest owns the driver. */
 if(*z->native.service_state!=1u)return -1;
 uint64_t ring=z->native.ring_descriptor[0];
 uint32_t count=(uint32_t)z->native.ring_descriptor[2];
 if(!ring || count<2u || count>8u)return -1;
 copy(z->format,format_image,sizeof z->format);
 copy(z->channels,channels_image,sizeof z->channels);
 copy(z->route,route_image,sizeof z->route);
 copy(z->mode,mode_image,sizeof z->mode);
 copy(z->go,go_image,sizeof z->go);
 copy(z->stop_payload,stop_image,sizeof z->stop_payload);
 z->ring.ring_count=count;z->ring.block_bytes=LAZ8_BLOCK_BYTES;z->ring.zero=0;
 z->ring.block_frames=LAZ8_BLOCK_FRAMES;z->ring.enable=1;
 z->ring.ring=ring;z->ring.callback=(uint64_t)(uintptr_t)live_audio_z8_block;z->ring.tail=0;
 z->ring.result=0;z->stop_posted=0;STORE(&z->stop_ack,0);z->stop_t=0;
 STORE(&z->head,0);STORE(&z->tail,0);STORE(&z->overrun,0);
 z->frames=0;z->anchor_us=0;z->unmuted=0;z->saved_valid=0;z->resume_bridge=0;
 STORE(&z->closed,0);z->phase=LAZ8_STARTING;
 static const uint32_t order[6]={3u,10u,0u,1u,8u,5u}; /* 104,10b,101,102,109,106 */
 void *payload[6]={z->format,z->channels,z->route,z->mode,&z->ring,z->go};
 for(unsigned i=0;i<6u;i++) {
  send(z,i,MSG_BASE+order[i],payload[i]);
  if(i==5u){z->msg[i].param=(uint64_t)(uintptr_t)start_complete;z->msg[i].tail=(uint64_t)(uintptr_t)z;}
  if(!z->native.post(&z->msg[i])) {
   __atomic_fetch_or(&z->closed,CALLBACK_CLOSED,__ATOMIC_ACQ_REL);
   if(!i)z->phase=LAZ8_IDLE;
   /* The core will stop/retry and retain the descriptor if any message queued. */
   return -1;
  }
 }
 z->phase=LAZ8_RUNNING;return 0;
}

static void stop_complete(uint32_t event,void *context) {
 struct live_audio_z8 *z=context;
 if(z && event==MSG_BASE+7u)STORE(&z->stop_ack,1u);
}
static int z8_stop(void *opaque) {
 struct live_audio_z8 *z=opaque;
 /* Closing admission and counting entered callbacks share one atomic word;
  * a callback that entered before this store must finish before release. */
 uint32_t gate=__atomic_fetch_or(&z->closed,CALLBACK_CLOSED,__ATOMIC_ACQ_REL);
 if(z->phase==LAZ8_IDLE)return (gate&~CALLBACK_CLOSED)?LAUDIO_BUSY:0;
 z->phase=LAZ8_STOPPING;
 uint64_t now=z->native.clock_us();
 if(!z->stop_posted){
  if(z->stop_t && now-z->stop_t<20000u)return LAUDIO_BUSY;
  send(z,6,MSG_BASE+7u,z->stop_payload);
  z->msg[6].param=(uint64_t)(uintptr_t)stop_complete;
  z->msg[6].tail=(uint64_t)(uintptr_t)z;
  STORE(&z->stop_ack,0);z->stop_t=now;
  if(!z->native.post(&z->msg[6])){z->stop_timeouts++;return LAUDIO_BUSY;}
  z->stop_posted=1;
 }
 /* state==1 alone is insufficient: queued start/stop messages may not yet
  * have run. 108's callback proves our stop was consumed, then idle and the
  * closed callback gate prove the allocation can be released. */
 if(!LOAD(&z->stop_ack)){
  if(now-z->stop_t>=2000000u){z->stop_timeouts++;z->stop_t=now;}
  return LAUDIO_BUSY;
 }
 if(*z->native.service_state!=1u){
  if(now-z->stop_t>=20000u){z->stop_posted=0;z->stop_t=0;}
  return LAUDIO_BUSY;
 }
 if(LOAD(&z->closed)&~CALLBACK_CLOSED)return LAUDIO_BUSY;
 restore_codec(z);
 z->phase=LAZ8_IDLE;z->stop_posted=0;return 0;
}

void live_audio_z8_ops(struct live_audio_z8 *z,struct live_audio_ops *o) {
 *o=(struct live_audio_ops){z,z8_allocate,z8_release,z8_start,z8_stop,1};
}

void live_audio_z8_block(const void *pcm,uint32_t bytes) {
 struct live_audio_z8 *z=live_audio_z8_context();
 if(!z)return;
 uint32_t gate=LOAD(&z->closed);
 do {if(gate&CALLBACK_CLOSED)return;}
 while(!__atomic_compare_exchange_n(&z->closed,&gate,gate+1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
 if(!z->stage || !pcm)goto done;
 if(bytes!=LAZ8_BLOCK_BYTES){z->dropped++;goto done;}
 uint32_t h=LOAD(&z->head),t=LOAD(&z->tail);
 if(h-t>=LAZ8_SLOTS){STORE(&z->overrun,1);z->dropped++;goto done;}
 uint32_t slot=h%LAZ8_SLOTS;
 z->stamp_us[slot]=z->native.clock_us();
 copy_block(z->stage+slot*LAZ8_BLOCK_BYTES,pcm);
 STORE(&z->head,h+1u);
 done:__atomic_fetch_sub(&z->closed,1u,__ATOMIC_RELEASE);
}

__attribute__((noinline))
int live_audio_z8_poll(struct live_audio *a,struct live_audio_z8 *z) {
 if(z->phase!=LAZ8_RUNNING)return LAUDIO_OK;
 uint32_t h=LOAD(&z->head),t=LOAD(&z->tail);
 if(!z->unmuted && LOAD(&z->go[7])) {
  if(!z->go[6] || *z->native.service_state!=5u)return LAUDIO_UNAVAILABLE;
  /* GO's callback follows its codec defaults. Do not wait another 500 ms
   * for an already-muted block, and never perform codec I/O in that callback. */
  int bad=0;
  for(unsigned c=0;c<2u;c++)bad|=z->native.codec_read(0x15u+c,&z->saved[c]);
  z->saved_valid=!bad;if(bad)z->codec_errors++;
  for(unsigned c=0;c<2u;c++)if(z->native.codec_write(0x15u+c,z->adc_volume)){z->codec_errors++;bad=1;}
  z->unmuted=1;
  z->anchor_us=z->native.clock_us();z->frames=0;
  STORE(&z->tail,h);t=h; /* All completed blocks predate un-mute. */
  if(bad)return LAUDIO_UNAVAILABLE;
 }
 if(!z->unmuted)return LAUDIO_OK;
 int result=LAUDIO_OK;
 if(LOAD(&z->overrun)) {
  /* Lost blocks: drop what is staged and re-anchor; push reports the gap. */
  STORE(&z->tail,h);STORE(&z->overrun,0);z->frames=0;z->anchor_us=z->native.clock_us();
  if(!a->frozen)live_audio_thaw(a);
  return LAUDIO_GAP;
 }
 while(t!=h) {
  uint32_t slot=t%LAZ8_SLOTS;
  if(!z->frames) {
   /* anchor_us is the un-mute/restart threshold until the first valid block.
    * A partial block spanning that point must not become pre-roll history. */
   if(z->stamp_us[slot]<z->anchor_us+LAZ8_BLOCK_US){STORE(&z->tail,++t);continue;}
   z->anchor_us=z->stamp_us[slot]-LAZ8_BLOCK_US;
  }
  /* Sample clock, not callback jitter: each block is LAZ8_BLOCK_US. */
  uint64_t first=z->anchor_us+z->frames/LIVE_AUDIO_RATE*1000000u
   +(z->frames%LIVE_AUDIO_RATE)*1000000u/LIVE_AUDIO_RATE;
  const uint8_t *pcm=z->stage+slot*LAZ8_BLOCK_BYTES;
  if(z->resume_bridge){int e=live_audio_bridge_silence(a,first);if(e)return e;z->resume_bridge=0;}
  int r=live_audio_push(a,pcm,LAZ8_BLOCK_FRAMES,LAUDIO_S16LE,first);
  if(r==LAUDIO_GAP)r=live_audio_push(a,pcm,LAZ8_BLOCK_FRAMES,LAUDIO_S16LE,first);
  if(r==LAUDIO_STATE)r=LAUDIO_OK; /* frozen: keep draining, keep the clock */
  if(r && result==LAUDIO_OK)result=r;
  z->frames+=LAZ8_BLOCK_FRAMES;z->blocks++;
  STORE(&z->tail,++t);
 }
 return result;
}

/* Do not use live_audio_stop/start: those discard marked pre-roll PCM.
 * This keeps the allocation and core active flag while native108 closes
 * and drains its callbacks. No SFX operation or user mute is changed here. */
int live_audio_z8_pause(struct live_audio *a,struct live_audio_z8 *z){
 if(!a->active)return LAUDIO_STATE;
 if(z->phase==LAZ8_RUNNING && !a->frozen && !LOAD(&a->cancelled)){int e=live_audio_z8_poll(a,z);if(e)return e;}
 return z8_stop(z);
}
int live_audio_z8_resume(struct live_audio *a,struct live_audio_z8 *z){
 if(!a->active || a->frozen || LOAD(&a->cancelled) || z->phase!=LAZ8_IDLE)return LAUDIO_STATE;
 int e=z8_start(z);if(e)return LAUDIO_UNAVAILABLE;
 z->resume_bridge=1;return LAUDIO_OK;
}
