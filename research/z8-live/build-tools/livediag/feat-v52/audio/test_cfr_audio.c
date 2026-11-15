/* Independent cross-module check: candidate stream clock plus v52 audio core. */
#define main original_stream_test_main
#include "../timing/test_cfr_stream.c"
#undef main
#include "work/src-fix37/audio/live_audio.h"
static int aa(void *v,uint32_t n,void **d,uint8_t **p,uint32_t *actual){(void)v;*d=*p=malloc(n);*actual=n;return !*p;}
static void af(void *v,void *d){(void)v;free(d);}
static int az(void *v){(void)v;return 0;}
int main(void){
 const unsigned phases[]={0,1,124,125,999,33333,33366};
 const unsigned presses[]={750,1500,2000,2500,2523};
 unsigned cases=0;int16_t *pcm=calloc(LIVE_AUDIO_CAPACITY*2u,sizeof(*pcm));assert(pcm);
 for(unsigned p=0;p<sizeof(phases)/sizeof(phases[0]);p++)
 for(unsigned q=0;q<sizeof(presses)/sizeof(presses[0]);q++){
  init();uint64_t epoch=1000000u+phases[p];
  for(unsigned ms=0;ms<=5000;ms++){
   uint64_t t=epoch+(uint64_t)ms*1000u;
   if(ms%50u==0)signal_(t,t);
   if(ms==presses[q])assert(la_full_press(&c,t));
   tick_(t);
  }
  assert(s.job==2&&s.frozen_count);
  uint64_t span=s.clip_end-s.clip_start;
  assert(span<=4000000u);
  assert(s.clip_start<=s.clip_press&&s.clip_press<s.clip_end);
  assert(s.last_duration_us==33366u||s.last_duration_us==33367u);
  uint64_t video30=span*30u,exact30=(uint64_t)s.frozen_count*1001000u;
  assert(video30>exact30?video30-exact30<30u:exact30-video30<30u);
  struct live_audio a;struct live_audio_ops ops={0,aa,af,az,az,1};
  live_audio_init(&a,&ops);assert(!live_audio_start(&a));
  assert(!live_audio_mark(&a,s.clip_press));
  /* End with at least 1 ms of samples beyond the video end. */
  uint64_t end=s.clip_end+1000u,start=end>4000000u?end-4000000u:0;
  assert(!live_audio_push(&a,pcm,LIVE_AUDIO_CAPACITY,LAUDIO_S16LE,start));
  assert(!live_audio_freeze(&a,(struct live_audio_window){s.clip_start,s.clip_end}));
  struct live_audio_info info;uint32_t got;
  assert(!live_audio_get(&a,0,0,0,&got,&info));
  assert(info.window_begin_us==s.clip_start&&info.window_end_us==s.clip_end);
  uint64_t a30=(uint64_t)info.frames*30u,v30=(uint64_t)s.frozen_count*48048u;
  assert(a30>v30?a30-v30<=30u:v30-a30<=30u); /* <= one 48 kHz sample */
  /* The Apple marker interval selected by finish() contains the press. */
  unsigned marker=0;while(marker+1<s.frozen_count&&s.frozen[marker+1].pts_us<=s.clip_press)marker++;
  uint64_t marker_end=marker+1<s.frozen_count?s.frozen[marker+1].pts_us:s.clip_end;
  assert(s.frozen[marker].pts_us<=s.clip_press&&s.clip_press<marker_end);
  assert(marker_end-s.frozen[marker].pts_us==33366u||marker_end-s.frozen[marker].pts_us==33367u);
  assert(!live_audio_stop(&a));stop_();cases++;
 }
 free(pcm);assert(allocations==frees);
 printf("PASS %u CFR epoch/press windows, matching PCM to <=1 sample and Apple marker containment\n",cases);
 return 0;
}
