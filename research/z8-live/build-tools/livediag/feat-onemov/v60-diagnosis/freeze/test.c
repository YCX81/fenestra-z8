
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define LIVE_MAX_FRAMES 128
#define AU_RING_SLOTS 256
#define LIVE_BITSTREAM_BYTES 65536
#define STORE(p,v) (*(p)=(v))
#define LA_FROZEN 5
#define LA_REQUESTED 1
#define LA_OVERFLOW 2
struct au_entry{uint64_t pts;unsigned size;};
struct au_ring{unsigned head,count;struct au_entry entries[AU_RING_SLOTS];};
typedef struct{uint8_t*annexb;unsigned bytes;uint64_t pts_us;}live_input_au;
struct la_capture{unsigned phase;};
struct live_stream{struct au_ring ring;uint64_t clip_start,clip_end,clip_press;uint8_t frozen_bytes[LIVE_BITSTREAM_BYTES];live_input_au frozen[LIVE_MAX_FRAMES];unsigned frozen_count,frozen_size,last_duration_us;uint64_t snapshot_first,snapshot_last;unsigned snapshot_encoded,encoded,job,error;};
typedef struct{unsigned count;uint64_t pts[LIVE_MAX_FRAMES];}live_mov;
static int au_ring_find_start(const struct au_ring*r,uint64_t pts,unsigned*out){for(unsigned i=0;i<r->count;i++)if(r->entries[(r->head+i)%AU_RING_SLOTS].pts==pts){*out=i;return 0;}return -1;}
static int au_ring_copy(const struct au_ring*r,unsigned i,uint8_t*out,unsigned cap){unsigned n=r->entries[(r->head+i)%AU_RING_SLOTS].size;if(n>cap)return -1;memset(out,(int)i,n);return (int)n;}
static uint64_t tick(unsigned index){return UINT64_C(1000000)+(uint64_t)index*1001000/30;}
static void freeze(struct la_capture *c,struct live_stream *s) {
 unsigned start;if(au_ring_find_start(&s->ring,s->clip_start,&start))goto fail;
 if(s->ring.entries[(s->ring.head+start)%AU_RING_SLOTS].pts!=s->clip_start)goto fail;
 unsigned count=0,bytes=0;
 for(unsigned i=start;i<s->ring.count;i++){
  const struct au_entry *e=&s->ring.entries[(s->ring.head+i)%AU_RING_SLOTS];
  if(e->pts>=s->clip_end)break;
  if(count==LIVE_MAX_FRAMES || e->size>LIVE_BITSTREAM_BYTES-bytes)goto fail;
  if(au_ring_copy(&s->ring,i,s->frozen_bytes+bytes,LIVE_BITSTREAM_BYTES-bytes)<0)goto fail;
  s->frozen[count++]=(live_input_au){s->frozen_bytes+bytes,e->size,e->pts};bytes+=e->size;
 }
 if(!count||s->frozen[count-1].pts_us<s->clip_press)goto fail;
 if(count>3u&&count%3u==1u&&s->frozen[count-2].pts_us>=s->clip_press){count--;bytes-=(unsigned)s->frozen[count].bytes;s->clip_end=s->frozen[count].pts_us;} /* v60: stock player end-of-stream */
 s->frozen_count=count;s->frozen_size=bytes;s->last_duration_us=s->clip_end-s->frozen[count-1].pts_us;
 s->snapshot_first=s->frozen[0].pts_us;s->snapshot_last=s->frozen[count-1].pts_us;
 s->snapshot_encoded=s->encoded;STORE(&c->phase,LA_FROZEN);STORE(&s->job,2);return;
 fail:s->error=LA_OVERFLOW;STORE(&s->job,0);STORE(&c->phase,LA_REQUESTED);
}
static int is_cfr(const live_mov*m,unsigned duration){
 for(unsigned i=1;i<m->count;i++){
  uint64_t a=(m->pts[i]-m->pts[0])*30u,b=(uint64_t)i*1001000u;
  if(a>b?a-b>=30u:b-a>=30u)return 0;
 }
 uint64_t a=(uint64_t)duration*30u,b=(uint64_t)m->count*1001000u;
 return (a>b?a-b:b-a)<30u;
}

int main(void){unsigned cases=0,trimmed=0,guarded=0;static struct live_stream s;
for(unsigned head_case=0;head_case<3;head_case++)for(unsigned phase=0;phase<30;phase++)for(unsigned count=1;count<=128;count++)for(unsigned press_frame=0;press_frame<count;press_frame++)for(unsigned edge=0;edge<2;edge++){
 if(edge&&press_frame+1==count)continue;
 memset(&s,0,sizeof s);struct la_capture c={0};unsigned prefix=3;s.ring.head=head_case==0?0:head_case==1?255:200;s.ring.count=prefix+count;
 for(unsigned i=0;i<s.ring.count;i++)s.ring.entries[(s.ring.head+i)%AU_RING_SLOTS]=(struct au_entry){tick(phase+i),i+1};
 s.clip_start=tick(phase+prefix);s.clip_end=tick(phase+prefix+count);uint64_t old_end=s.clip_end;s.clip_press=tick(phase+prefix+press_frame)+edge;s.encoded=123;
 freeze(&c,&s);assert(c.phase==LA_FROZEN&&s.job==2);assert(s.frozen_count==count||s.frozen_count+1==count);assert(s.frozen_count>0);
 assert(s.clip_start<=s.clip_press&&s.clip_press<s.clip_end);assert(s.snapshot_first==s.clip_start);assert(s.snapshot_last>=s.clip_press);assert(s.snapshot_encoded==s.encoded);
 unsigned sum=0;live_mov m={.count=s.frozen_count};for(unsigned i=0;i<s.frozen_count;i++){assert(s.frozen[i].pts_us==tick(phase+prefix+i));assert(s.frozen[i].annexb==s.frozen_bytes+sum);sum+=s.frozen[i].bytes;m.pts[i]=s.frozen[i].pts_us;}
 assert(sum==s.frozen_size);assert(s.last_duration_us>=33366&&s.last_duration_us<=33367);assert(s.snapshot_last+s.last_duration_us==s.clip_end);
 unsigned duration=(unsigned)(s.clip_end-s.clip_start);assert(is_cfr(&m,duration));
 uint64_t lo=(s.clip_start*48000+999999)/1000000,hi=(s.clip_end*48000+999999)/1000000;assert(hi>lo);
 uint64_t exact=(uint64_t)s.frozen_count*8008;uint64_t scaled=(hi-lo)*5;assert(scaled>exact?scaled-exact<=5:exact-scaled<=5);
 if(s.frozen_count<count){trimmed++;assert(old_end-s.clip_end>=33366&&old_end-s.clip_end<=33367);assert(s.frozen_count%3==0);}else if(count>3&&count%3==1)guarded++;
 cases++;
}
printf("PASS: %u actual-freeze cases; trimmed=%u, near-press guard=%u; all successful phases, bytes, CFR, press containment, audio-window counts consistent\n",cases,trimmed,guarded);return 0;}
