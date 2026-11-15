#include "audio/live_audio_z8.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct live_audio_z8 z;
static struct live_audio a;
static uint32_t service=1,queued,consumed,allocs,frees,fail_post;
static uint64_t now=1000000,ring[3]={0x12340000,0,5};
static struct laz8_msg *queue[64];
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int in_callback,release_callback,block_clock;
static pthread_t callback_thread;
static uint8_t pcm[LAZ8_BLOCK_BYTES];
struct live_audio_z8 *live_audio_z8_context(void){return &z;}
static int post(struct laz8_msg *m){
 if(fail_post==m->id){fail_post=0;return 0;}
 assert(queued<64);queue[queued++]=m;return 1;
}
static int read_codec(uint32_t reg,uint16_t *v){assert(reg==0x15||reg==0x16);*v=0x100;return 0;}
static int write_codec(uint32_t reg,uint32_t v){assert(reg==0x15||reg==0x16);(void)v;return 0;}
static void sleep_(uint32_t ms){(void)ms;assert(!"stopping must never block the worker");}
static int allocate(void*v,uint32_t n,void**d,uint8_t**p,uint32_t*actual){
 (void)v;*d=calloc(1,n);assert(*d);*p=*d;*actual=n;allocs++;return 0;
}
static void release(void*v,void*d){
 (void)v;assert(z.phase==LAZ8_IDLE);assert((z.closed&0x7fffffff)==0);
 free(d);frees++;
}
static uint64_t clock_(void){
 if(block_clock&&pthread_equal(pthread_self(),callback_thread)){
  pthread_mutex_lock(&mu);in_callback=1;pthread_cond_broadcast(&cv);
  while(!release_callback)pthread_cond_wait(&cv,&mu);
  pthread_mutex_unlock(&mu);
 }
 return now;
}
int live_audio_native_alloc(void*v,uint32_t n,void**d,uint8_t**p,uint32_t*actual){return allocate(v,n,d,p,actual);}
void live_audio_native_free(void*v,void*d){release(v,d);}
uint64_t live_audio_clock_us(void){return clock_();}
static void consume_one(void){
 assert(consumed<queued);struct laz8_msg*m=queue[consumed++];uint8_t*p=m->payload;
 switch(m->id){
 case 0x42040104:p[0x20]=1;service=4;break;
 case 0x4204010b:p[4]=1;break;
 case 0x42040101:p[0xd]=1;break;
 case 0x42040102:p[0xc]=1;break;
 case 0x42040109:p[0x30]=1;break;
 case 0x42040106:p[6]=1;service=5;if(m->param)((void(*)(uint32_t,void*))(uintptr_t)m->param)(m->id,(void*)(uintptr_t)m->tail);break;
 case 0x42040108:
  p[0]=1;service=1;
  if(m->param)((void(*)(uint32_t,void*))(uintptr_t)m->param)(m->id,(void*)(uintptr_t)m->tail);
  break;
 default:assert(0);
 }
}
static void consume_all(void){while(consumed<queued)consume_one();}
static void init(void){
 assert(allocs==frees);memset(&z,0,sizeof z);live_audio_z8_init(&z,0);
 z.native=(struct laz8_native){post,read_codec,write_codec,sleep_,&service,ring,allocate,release,clock_};
 struct live_audio_ops ops;live_audio_z8_ops(&z,&ops);live_audio_init(&a,&ops);
 service=1;queued=consumed=0;fail_post=0;now+=1000000;
}
static void *copy_thread(void*v){(void)v;callback_thread=pthread_self();live_audio_z8_block(pcm,sizeof pcm);return 0;}
int main(void){
 init();assert(!live_audio_start(&a));assert(a.active&&a.descriptor&&queued==6);
 uint8_t *stage=z.stage;assert(live_audio_stop(&a)==LAUDIO_BUSY);
 assert(service==1&&a.active&&a.descriptor&&z.phase==LAZ8_STOPPING);
 assert(frees==0);consume_all();assert(z.stage==stage&&z.channels[0]==0);
 assert(!live_audio_stop(&a));assert(!a.active&&!a.descriptor&&allocs==frees);
 puts("PASS queued start + idle driver cannot fake stop completion; payload replies preserve neighbors");

 init();assert(!live_audio_start(&a));consume_all();fail_post=0x42040108;
 assert(live_audio_stop(&a)==LAUDIO_BUSY);assert(a.active&&a.descriptor);
 now+=20000;assert(live_audio_stop(&a)==LAUDIO_BUSY);
 uint32_t q=queued;now+=3000000;assert(live_audio_stop(&a)==LAUDIO_BUSY);
 assert(queued==q&&a.active&&a.descriptor&&z.stop_timeouts==2);
 consume_all();assert(!live_audio_stop(&a));assert(allocs==frees);
 puts("PASS rejected stop retries; unanswered stop retains message/allocation and never reposts unsafely");

 init();fail_post=0x4204010b;assert(live_audio_start(&a)==LAUDIO_UNAVAILABLE);
 assert(a.active&&a.descriptor&&a.cancelled);consume_all();assert(!live_audio_stop(&a));
 assert(allocs==frees);assert(!live_audio_start(&a));consume_all();
 assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(!live_audio_stop(&a));
 puts("PASS partial start failure retains resources until stop, then restarts");

 init();assert(!live_audio_start(&a));consume_all();
 block_clock=1;in_callback=release_callback=0;pthread_t thread;
 assert(!pthread_create(&thread,0,copy_thread,0));
 pthread_mutex_lock(&mu);while(!in_callback)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);
 assert(live_audio_stop(&a)==LAUDIO_BUSY);consume_all();assert(service==1&&z.stop_ack);
 assert(live_audio_stop(&a)==LAUDIO_BUSY&&a.active&&a.descriptor);
 pthread_mutex_lock(&mu);release_callback=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
 assert(!pthread_join(thread,0));block_clock=0;
 assert(!live_audio_stop(&a));assert(allocs==frees);
 live_audio_z8_block(pcm,sizeof pcm);assert(allocs==frees);
 puts("PASS callback in flight drains before free; closed callback after release is inert");
 return 0;
}
