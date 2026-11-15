#include "runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static struct live_runtime r;
static uint32_t service=1,freed,allocated,nq,wakes,inject;
static uint64_t now=10000000,ring[3]={0x12340000,0,5};
static struct laz8_msg *q[16];
static struct laz8_msg held;
static uintptr_t envelope[3],tone[5];
static uint32_t replies,outerdone;
uint32_t test_word(uintptr_t p){if(p==0xa02d4cb48)return service;if(p==0xa03bd062c||p==0xa038feb74)return 0;return *(uint32_t*)p;}
uintptr_t test_ptr(uintptr_t p){if(p==0xa038febb8||p==0xa038feb98)return 0;if(p==0xa038fea70)return 0x1234;return *(uintptr_t*)p;}
uint8_t test_byte(uintptr_t p){assert(p==0xa03bc7008||p==0xa038fe9f1);return 0;}
void *live_ui_engine_get(void){return &r;}
uint32_t live_shutter_mute_get(void){return 0;}
uint64_t live_audio_clock_us(void){return now;}
struct live_audio_z8 *live_audio_z8_context(void){return &r.audio_z8;}
int live_audio_native_alloc(void*x,uint32_t n,void**d,uint8_t**p,uint32_t*actual){(void)x;*d=calloc(1,n);assert(*d);*p=*d;*actual=n;allocated++;return 0;}
void live_audio_native_free(void*x,void*d){(void)x;assert(r.audio_z8.phase==LAZ8_IDLE);assert(!(r.audio_z8.closed&0x7fffffff));free(d);freed++;}
static void reply(uint32_t ev,void*p){assert(p);assert(ev==0x04102004||ev==0x04102005);replies++;}
static void outer(uint32_t ev,void*p){assert(ev==0x04102000&&p==&held);outerdone++;}
static void new_holder(void){
 memset(&held,0,sizeof held);memset(envelope,0,sizeof envelope);memset(tone,0,sizeof tone);
 ((uint32_t*)tone)[3]=2;tone[3]=(uintptr_t)reply;envelope[0]=(uintptr_t)tone;
 held.id=0x04100030;held.payload=envelope;held.param=(uintptr_t)outer;held.tail=(uintptr_t)&held;
 r.shutter.held=(uintptr_t)&held;r.shutter.tone=2;r.shutter.generation++;r.shutter.began=now;r.shutter.phase=1;
}
static int post(struct laz8_msg*m){
 if(inject && m->id==0x4204010b){inject=0;new_holder();return 0;}
 assert(nq<16);q[nq++]=m;return 1;
}
static int codec_read(uint32_t n,uint16_t*v){(void)n;*v=0x100;return 0;}
static int codec_write(uint32_t n,uint32_t v){(void)n;(void)v;return 0;}
static void sleep_(uint32_t n){(void)n;assert(0);}
static int notify(void*task,void*m){assert(task==(void*)0x1234&&m==&r.shutter.wake);wakes++;return 0;}
static void unexpected_dispatch(void*m){(void)m;assert(!"canceled replacement must never reach sound driver");}
void *test_function(uintptr_t p){if(p==0x40c00bac)return(void*)notify;if(p==0x40e34b2c)return(void*)unexpected_dispatch;assert(0);return 0;}
static void consume(void){for(unsigned i=0;i<nq;i++){
 struct laz8_msg*m=q[i];if(m->id==0x42040104){service=4;((uint8_t*)m->payload)[32]=1;}
 if(m->id==0x42040106){service=5;((uint8_t*)m->payload)[6]=1;}
 if(m->id==0x42040108)service=1;
 if(m->param)((void(*)(uint32_t,void*))(uintptr_t)m->param)(m->id,(void*)(uintptr_t)m->tail);
 }nq=0;}
int main(void){
 r.magic=LIVE_RUNTIME_MAGIC;r.size=sizeof r;r.requested=1;live_audio_z8_init(&r.audio_z8,0);
 r.audio_z8.native=(struct laz8_native){post,codec_read,codec_write,sleep_,&service,ring,live_audio_native_alloc,live_audio_native_free,live_audio_clock_us};
 struct live_audio_ops ops;live_audio_z8_ops(&r.audio_z8,&ops);live_audio_init(&r.audio,&ops);
 assert(!live_audio_start(&r.audio));consume();assert(live_audio_z8_pause(&r.audio,&r.audio_z8)==LAUDIO_BUSY);consume();assert(!live_audio_z8_pause(&r.audio,&r.audio_z8));
 void*keep=r.audio.descriptor;r.shutter.phase=3;inject=1;assert(live_shutter_step(&r,1,now)==1);
 assert(r.shutter.phase==1&&r.audio.cancelled&&r.audio_z8.phase==LAZ8_STARTING);
 consume();assert(service==4&&!freed&&r.audio.descriptor==keep);
 assert(live_shutter_step(&r,1,now)==1&&wakes==1);live_shutter_dispatch(&r.shutter.wake);
 assert(r.shutter.phase==3&&replies==2&&outerdone==1&&!r.shutter.held);
 assert(live_shutter_step(&r,1,now)==1);
 // This is the blocking counterexample: old SH_DONE ignored STARTING and
 // never posted 108, leaving driver4 forever. Actual fixed backend must post.
 assert(r.audio_z8.phase==LAZ8_STOPPING&&nq==1&&q[0]->id==0x42040108);
 assert(!freed&&r.audio.descriptor==keep);
 r.audio_z8.closed|=1u;consume();assert(service==1&&r.audio_z8.stop_ack);
 assert(live_shutter_step(&r,1,now)==1 && r.audio.descriptor==keep&&!freed);
 r.audio_z8.closed&=0x80000000u;
 assert(live_shutter_step(&r,1,now)==0&&r.shutter.phase==0&&r.audio_z8.phase==LAZ8_IDLE);
 assert(!live_audio_stop(&r.audio)&&freed==allocated&&!r.audio.descriptor);
 puts("PASS actual adapter+backend: partial resume/new held/cancel DONE drives108, waits ACK+callback drain, retains allocation until normal cleanup");
}
