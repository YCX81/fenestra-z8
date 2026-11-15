#include "live_audio.h"
#define CANCEL(a) __atomic_load_n(&(a)->cancelled,__ATOMIC_ACQUIRE)
static uint64_t sample_floor(uint64_t us) {
 return (us/1000000u)*LIVE_AUDIO_RATE+(us%1000000u)*LIVE_AUDIO_RATE/1000000u;
}
static uint64_t sample_ceil(uint64_t us) {
 return sample_floor(us)+(us%125u!=0); /* 48000 / 1e6 = 6/125 */
}
static uint64_t sample_us(uint64_t n) {
 return n/LIVE_AUDIO_RATE*1000000u+(n%LIVE_AUDIO_RATE)*1000000u/LIVE_AUDIO_RATE;
}
static void history_clear(struct live_audio *a) {
 a->count=0;a->marked=0;a->frozen=0;a->freeze_frames=0;
}
void live_audio_init(struct live_audio *a,const struct live_audio_ops *ops) {
 uint8_t *p=(uint8_t *)a;for(size_t i=0;i<sizeof(*a);i++)p[i]=0;
 if(ops)a->ops=*ops;
 a->duck=(struct live_audio_duck){50000,100000,5000,0};
}
int live_audio_start(struct live_audio *a) {
 if(a->active)return CANCEL(a)?LAUDIO_CANCELLED:LAUDIO_OK;
 if(!a->ops.verified_photo_backend || !a->ops.allocate || !a->ops.release ||
    !a->ops.start_capture || !a->ops.stop_capture)return LAUDIO_UNAVAILABLE;
 __atomic_store_n(&a->cancelled,0,__ATOMIC_RELEASE);
 void *d=0;uint8_t *p=0;uint32_t actual=0;
 if(a->ops.allocate(a->ops.opaque,LIVE_AUDIO_BYTES,&d,&p,&actual) ||
    !d || !p || ((uintptr_t)p&1) || actual<LIVE_AUDIO_BYTES) {
  if(d)a->ops.release(a->ops.opaque,d);
  return LAUDIO_ALLOC;
 }
 if(CANCEL(a)){a->ops.release(a->ops.opaque,d);return LAUDIO_CANCELLED;}
 if(a->ops.start_capture(a->ops.opaque)) {
  a->ops.release(a->ops.opaque,d);return LAUDIO_UNAVAILABLE;
 }
 a->descriptor=d;a->ring=(int16_t *)p;history_clear(a);a->active=1;
 if(CANCEL(a)){live_audio_stop(a);return LAUDIO_CANCELLED;}
 return 0;
}
void live_audio_cancel(struct live_audio *a) {
 __atomic_store_n(&a->cancelled,1,__ATOMIC_RELEASE);
}
void live_audio_stop(struct live_audio *a) {
 live_audio_cancel(a);
 if(a->active)a->ops.stop_capture(a->ops.opaque);
 a->active=0;history_clear(a);
 if(a->descriptor)a->ops.release(a->ops.opaque,a->descriptor);
 a->descriptor=0;a->ring=0;
}
int live_audio_set_duck(struct live_audio *a,struct live_audio_duck d) {
 if(a->frozen)return LAUDIO_STATE;
 if(d.before_us>1000000 || d.after_us>1000000 || d.ramp_us>100000 ||
    d.gain_q15>32768)return LAUDIO_RANGE;
 a->duck=d;return 0;
}
int live_audio_mark(struct live_audio *a,uint64_t t) {
 if(CANCEL(a))return LAUDIO_CANCELLED;
 if(!a->active || a->frozen || a->marked)return LAUDIO_STATE;
 a->press_us=t;a->exposure_us=t;a->marked=1;return 0;
}
int live_audio_exposure(struct live_audio *a,uint64_t t) {
 if(CANCEL(a))return LAUDIO_CANCELLED;
 if(!a->active || !a->marked || a->frozen)return LAUDIO_STATE;
 a->exposure_us=t;return 0;
}
int live_audio_push(struct live_audio *a,const void *pcm,uint32_t n,
                    enum live_audio_input fmt,uint64_t us) {
 if(CANCEL(a))return LAUDIO_CANCELLED;
 if(!a->active || a->frozen)return LAUDIO_STATE;
 if(!pcm || !n || n>LIVE_AUDIO_CAPACITY || (fmt!=2 && fmt!=3 && fmt!=4))
  return LAUDIO_RANGE;
 uint64_t at=sample_floor(us);
 if(a->count) {
  uint64_t delta=at>a->next_index?at-a->next_index:a->next_index-at;
  if(delta>1) {history_clear(a);return LAUDIO_GAP;}
  at=a->next_index; /* quantization tolerance, never gradual timestamp drift */
 }
 const uint8_t *p=pcm;
 for(uint32_t i=0;i<n;i++) {
  uint32_t slot=(uint32_t)((at+i)%LIVE_AUDIO_CAPACITY)*2;
  for(unsigned ch=0;ch<2;ch++) {
   /* Intentional truncate to signed 16-bit; input format must be verified. */
   unsigned skip=(unsigned)fmt-2;
   a->ring[slot+ch]=(int16_t)((uint16_t)p[skip]|(uint16_t)p[skip+1]<<8);
   p+=(unsigned)fmt;
  }
 }
 if(CANCEL(a)) {history_clear(a);return LAUDIO_CANCELLED;}
 a->next_index=at+n;
 a->count=a->count+n>LIVE_AUDIO_CAPACITY?LIVE_AUDIO_CAPACITY:a->count+n;
 a->first_index=a->next_index-a->count;return 0;
}
int live_audio_freeze(struct live_audio *a,struct live_audio_window w) {
 if(CANCEL(a))return LAUDIO_CANCELLED;
 if(!a->active || a->frozen || !a->marked)return LAUDIO_STATE;
 if(w.end_us<=w.begin_us || w.end_us-w.begin_us>4000000 ||
    a->press_us<w.begin_us || a->press_us>=w.end_us)return LAUDIO_RANGE;
 uint64_t lo=sample_ceil(w.begin_us),hi=sample_ceil(w.end_us);
 if(!a->count || hi>a->next_index || hi<=lo || a->first_index>=hi)return LAUDIO_GAP; /* v29: a start before the history is silence */
 a->freeze_first=lo;a->freeze_frames=(uint32_t)(hi-lo);a->window=w;
 a->frozen=1;return 0;
}
static uint32_t gain(const struct live_audio *a,uint64_t us) {
 uint64_t lo=a->exposure_us>a->duck.before_us?a->exposure_us-a->duck.before_us:0;
 uint64_t hi=UINT64_MAX-a->exposure_us<a->duck.after_us?UINT64_MAX:a->exposure_us+a->duck.after_us;
 uint32_t g=a->duck.gain_q15,r=a->duck.ramp_us;
 if(us>=lo && us<hi)return g;
 uint64_t d=us<lo?lo-us:us-hi;
 if(!r || d>=r)return 32768;
 return g+(uint32_t)((32768u-g)*d/r);
}
int live_audio_get(struct live_audio *a,uint32_t off,int16_t *out,uint32_t cap,
                   uint32_t *got,struct live_audio_info *info) {
 if(got)*got=0;
 if(CANCEL(a))return LAUDIO_CANCELLED;
 if(!a->active || !a->frozen)return LAUDIO_STATE;
 if(!got || !info || (cap && !out) || off>a->freeze_frames)return LAUDIO_RANGE;
 *info=(struct live_audio_info){LIVE_AUDIO_RATE,2,16,a->freeze_frames,
  sample_us(a->freeze_first),a->window.begin_us,a->window.end_us};
 uint32_t n=a->freeze_frames-off;if(n>cap)n=cap;
 for(uint32_t i=0;i<n;i++) {
  uint64_t index=a->freeze_first+off+i;
  uint32_t g=gain(a,sample_us(index)),slot=(uint32_t)(index%LIVE_AUDIO_CAPACITY)*2;
  for(unsigned ch=0;ch<2;ch++)out[2*i+ch]=index<a->first_index?0:(int16_t)((int32_t)a->ring[slot+ch]*(int32_t)g/32768);
 }
 if(CANCEL(a))return LAUDIO_CANCELLED; /* caller must discard out on any error */
 *got=n;return 0;
}
void live_audio_thaw(struct live_audio *a) {history_clear(a);}
