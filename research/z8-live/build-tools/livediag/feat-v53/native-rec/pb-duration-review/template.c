
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include "preview_reader.h"
#define LOAD(p) (*(p))
#define STORE(p,v) (*(p)=(v))
#define PB_AUDIO_SERVICE service
#define PB_AUDIO_RING 0xa10000000ull
#define PB_AUDIO_BLOCK_BYTES 192000u
#define PB_AUDIO_BLOCKS 5u
#define PB_SCENE_DATA_BYTES 4096u
#define FX_OK 0
static uint64_t now;static uint32_t service,posts,last_post,hidden,read_fail,reads,decoded;
static uint64_t now_us(void){return now;}
static uint32_t preference(uint32_t x){(void)x;return 0;} /* muted preference remains valid */
static uint32_t connection_state(uint32_t x){(void)x;return 0;}
typedef struct {
 LpPreview preview;LpClip clip;uint64_t start_us,audio_t;uint32_t key_epoch;
 uint32_t audio_stage,audio_stop,audio_bytes,audio_pending,audio_ack,audio_errors,audio_stop_retries,audio_gate,audio_plays,audio_skipped,audio_reason;
 uint32_t shutdown,playing,blocked_key_epoch,last_frame,clip_mode,decode_errors,current_id,frames;
 int preview_fd;uint8_t audio_payload[32],jpeg[32],i420[32];void *decoder,*pixels;int scene,commands[4];
} PbRuntime;
static void audio_release(PbRuntime*r){r->audio_gate&=~2u;}
static void audio_begin(PbRuntime*r,int fd){(void)r;(void)fd;assert(0);}
static void audio_callback(uint32_t event,void*ctx){(void)event;(void)ctx;}
static void audio_post(PbRuntime*r,uint32_t id){posts++;last_post=id;r->audio_ack=0;r->audio_t=now;}
static void hide(PbRuntime*r){hidden++;r->playing=0;if(r->audio_stage)r->audio_stop=1;}
static int read_at(int fd,uint64_t at,void*p,uint32_t n){(void)fd;(void)at;(void)p;(void)n;reads++;return !read_fail;}
static int decode_stub(void*d,const void*j,uint32_t n,void*i,uint32_t w,uint32_t h){(void)d;(void)j;(void)n;(void)i;(void)w;(void)h;decoded++;return 0;}
static int ready_to_write(PbRuntime*r,uint32_t key){(void)r;(void)key;return 1;}
static int pb_scene_frame(int*s,int*c,void*p,uint32_t n,void*i,uint32_t fb){(void)s;(void)c;(void)p;(void)n;(void)i;(void)fb;return 0;}
static int current(uint32_t *id){*id=1;return 1;}
static int present(PbRuntime*r,uint32_t key){(void)r;(void)key;return 1;}
__INDEX__
__BODY__
static PbRuntime fresh(uint32_t nf){
 PbRuntime r={0};r.preview=(LpPreview){1024,682,45,1047552,3000015,9};r.key_epoch=9;
 r.playing=1;r.current_id=1;r.preview_fd=7;r.clip_mode=1;r.pixels=(void*)1;r.last_frame=44;
 r.audio_stage=4;r.audio_bytes=nf*4;r.audio_gate=2;r.start_us=1000000;r.audio_t=1000000;
 for(unsigned i=0;i<46;i++)r.clip.at[i]=i*128u;
 now=1000000;hidden=posts=last_post=reads=decoded=read_fail=0;service=2;return r;
}
int main(void){
 unsigned cases=0;PbRuntime r;
 /* Captured-file regression: old image EOS must not stop its last 69.729ms. */
 r=fresh(147347);now=r.start_us+3000015;frame(&r,now);assert(!hidden&&r.playing&&!r.audio_stop&&r.last_frame==44);cases++;
 now=r.audio_t+3069729;frame(&r,now);audio_step(&r);assert(!posts&&!hidden);cases++;
 now++;frame(&r,now);assert(hidden==1&&r.audio_stop);audio_step(&r);assert(last_post==0x43180016&&r.audio_stage==5);cases++;
 /* The final frame is presented once even if the worker skipped its interval. */
 r=fresh(152152);r.last_frame=43;now=r.start_us+3000015;frame(&r,now);assert(!hidden&&r.last_frame==44&&reads==1&&decoded==1&&r.frames==1);frame(&r,now+10);assert(reads==1);cases++;
 /* All key epochs and cancellation paths bypass the hold. */
 r=fresh(152152);r.key_epoch++;frame(&r,r.start_us+3000015);assert(hidden&&r.audio_stop);cases++;
 r=fresh(152152);r.audio_stop=1;frame(&r,r.start_us+3000015);assert(hidden);cases++;
 r=fresh(152152);r.last_frame=43;read_fail=1;frame(&r,r.start_us+3000015);assert(hidden&&r.audio_stop&&r.decode_errors==1);cases++;
 for(unsigned stage=0;stage<=8;stage++){if(stage==4)continue;r=fresh(152152);r.audio_stage=stage;frame(&r,r.start_us+3000015);assert(hidden);cases++;}
 /* No stretching old cadence, including mute (volume is zero above). */
 r=fresh(152152);r.last_frame=0;frame(&r,r.start_us+1000000);assert(r.last_frame==14);cases++;
 /* A delayed successful START uses the same video/audio epoch. */
 r=fresh(152152);r.audio_stage=3;r.audio_ack=0x43180015;r.audio_payload[20]=1;now+=7000;audio_step(&r);assert(r.audio_stage==4&&r.start_us==now&&r.audio_t==now&&r.audio_plays==1);cases++;
 /* Fractional sample endpoint: 1 stereo frame lasts 20.833us, never stop at20. */
 r=fresh(1);now=r.audio_t+20;audio_step(&r);assert(!posts);now++;audio_step(&r);assert(last_post==0x43180016);cases++;
 /* Four-second supported upper bound and fresh accurate header have no tail loss. */
 r=fresh(192000);frame(&r,r.start_us+3999999);assert(!hidden);frame(&r,r.start_us+4000000);assert(hidden);cases++;
 r=fresh(147347);r.preview.duration_us=3069765;now=r.audio_t+3069730;audio_step(&r);assert(last_post==0x43180016);frame(&r,now);assert(!hidden);frame(&r,r.start_us+3069765);assert(hidden);cases++;
 /* Native handoff closes audio admission and reaches ordinary stop despite EOF hold. */
 r=fresh(152152);r.audio_gate=3;now=r.start_us+3000015;frame(&r,now);audio_step(&r);assert(last_post==0x43180016&&r.audio_stop);frame(&r,now+1);assert(hidden);cases++;

 /* Independent adversarial state interleavings. */
 r=fresh(147347);r.audio_stage=3;r.audio_ack=0x43180015;r.audio_payload[20]=0;now+=9000;audio_step(&r);assert(r.audio_stage==5&&r.audio_errors==1&&r.start_us==1000000);frame(&r,r.start_us+3000015);assert(hidden);cases++;
 r=fresh(147347);r.audio_stage=3;r.audio_ack=0x43180015;r.audio_payload[20]=1;r.audio_gate=3;now+=9000;audio_step(&r);assert(r.audio_stage==5&&r.audio_stop&&r.audio_plays==0&&r.start_us==1000000);cases++;
 r=fresh(96000);now=r.audio_t+2000000;audio_step(&r);assert(r.audio_stage==5);frame(&r,now);assert(!hidden);frame(&r,r.start_us+3000015);assert(hidden);cases++;
 r=fresh(147347);r.audio_stage=0;r.clip_mode=0;frame(&r,r.start_us+3000015);assert(hidden&&posts==0);cases++;
 for(unsigned nf=144001;nf<=144192;nf++) { r=fresh(nf);uint64_t end=((uint64_t)nf*1000000+47999)/48000;now=r.audio_t+end-1;frame(&r,now);audio_step(&r);assert(!hidden&&!posts);now++;frame(&r,now);audio_step(&r);assert(hidden&&last_post==0x43180016);cases++; }
 printf("PASS %u actual-source frame/audio combinations; EOF hold, original cadence, last frame, cancellation/failure, delayed START, ceil sample endpoint, mute, 4s bound\n",cases);
}
