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
 a->duck=(struct live_audio_duck){0,0,0,32768};
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
 a->descriptor=d;a->ring=(int16_t *)p;history_clear(a);a->active=1;
 if(a->ops.start_capture(a->ops.opaque)) {
  /* A queued start may not have reached the driver yet. Retain its payloads
   * and callback staging allocation until stop positively acknowledges. */
  live_audio_stop(a);return LAUDIO_UNAVAILABLE;
 }
 if(CANCEL(a)){live_audio_stop(a);return LAUDIO_CANCELLED;}
 return 0;
}
void live_audio_cancel(struct live_audio *a) {
 __atomic_store_n(&a->cancelled,1,__ATOMIC_RELEASE);
}
int live_audio_stop(struct live_audio *a) {
 live_audio_cancel(a);
 if(a->active && a->ops.stop_capture(a->ops.opaque))return LAUDIO_BUSY;
 a->active=0;history_clear(a);
 if(a->descriptor)a->ops.release(a->ops.opaque,a->descriptor);
 a->descriptor=0;a->ring=0;return LAUDIO_OK;
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
 unsigned skip=(unsigned)fmt-2u,step=(unsigned)fmt;
 uint32_t pos=(uint32_t)(at%LIVE_AUDIO_CAPACITY),rem=n;
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
 int aln4=(((uintptr_t)a->ring)&3u)==0u;
#else
 int aln4=0;
#endif
 while(rem) {
  uint32_t seg=LIVE_AUDIO_CAPACITY-pos;if(seg>rem)seg=rem;
  int16_t *dst=a->ring+pos*2u;
  typedef uint32_t __attribute__((may_alias)) la_u32a;
  for(uint32_t i=0;i<seg;i++) {
   uint32_t l=(uint32_t)p[skip]|((uint32_t)p[skip+1u]<<8);p+=step;
   uint32_t r=(uint32_t)p[skip]|((uint32_t)p[skip+1u]<<8);p+=step;
   if(aln4)((la_u32a *)(void *)dst)[i]=l|(r<<16);
   else{dst[2u*i]=(int16_t)l;dst[2u*i+1u]=(int16_t)r;}
  }
  pos=0;rem-=seg;
 }
 if(CANCEL(a)) {history_clear(a);return LAUDIO_CANCELLED;}
 a->next_index=at+n;
 a->count=a->count+n>LIVE_AUDIO_CAPACITY?LIVE_AUDIO_CAPACITY:a->count+n;
 a->first_index=a->next_index-a->count;return 0;
}
static int freeze_impl(struct live_audio *a,struct live_audio_window w,int best) {
 if(CANCEL(a))return LAUDIO_CANCELLED;
 if(!a->active || a->frozen || !a->marked)return LAUDIO_STATE;
 if(w.end_us<=w.begin_us || w.end_us-w.begin_us>4000000 ||
    a->press_us<w.begin_us || a->press_us>=w.end_us)return LAUDIO_RANGE;
 uint64_t lo=sample_ceil(w.begin_us),hi=sample_ceil(w.end_us);
 if(hi<=lo || !a->count || (best?(a->next_index<=lo || a->first_index>=hi):(hi>a->next_index || a->first_index>lo)))return LAUDIO_GAP;
 a->freeze_first=lo;a->freeze_frames=(uint32_t)(hi-lo);a->window=w;
 a->frozen=1;return 0;
}
int live_audio_freeze(struct live_audio *a,struct live_audio_window w) {return freeze_impl(a,w,0);}
/* Fallback freeze when exact prefix or 50 ms tail block is slightly short:
 * keep all real captured audio overlapping [lo, hi) and zero-pad only missing edges. */
int live_audio_freeze_best(struct live_audio *a,struct live_audio_window w) {return freeze_impl(a,w,1);}
int live_audio_covers(const struct live_audio *a,uint64_t begin_us,uint64_t now_us,uint64_t max_lag_us) {
 if(!a->active || CANCEL(a) || !a->count || a->first_index>sample_ceil(begin_us))return 0;
 return now_us<=max_lag_us || sample_ceil(now_us-max_lag_us)<=a->next_index;
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
  for(unsigned ch=0;ch<2;ch++)out[2*i+ch]=(index<a->first_index || index>=a->next_index)?0:(int16_t)((int32_t)a->ring[slot+ch]*(int32_t)g/32768);
 }
 if(CANCEL(a))return LAUDIO_CANCELLED; /* caller must discard out on any error */
 *got=n;return 0;
}
void live_audio_thaw(struct live_audio *a) {history_clear(a);}
