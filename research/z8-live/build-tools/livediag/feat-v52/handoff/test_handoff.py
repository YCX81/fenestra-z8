#!/usr/bin/env python3
"""Run the actual C handoff/dispose/aux functions; mock native calls and time only."""
from pathlib import Path
import subprocess,tempfile,json
D=Path(__file__).resolve().parent;S=D/'work/src-fix37'
r=(S/'runtime.c').read_text();w=(S/'codec/live_w5enc.c').read_text();stream=(S/'capture/live_stream.c').read_text()
def section(s,a,b):return s[s.index(a):s.index(b,s.index(a))]
handoff=section(r,'void live_w5_bringup_notify','/* DMA callback context').replace('*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d980)','fake_vdi')
aux=section(stream,'void live_stream_release_aux','static int allocate')
tick=section(r,'  uint32_t hold=live_stock_hold','  uint64_t w4=now_us()')
preamble=r'''
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define FN(a,t) mock_delay
static struct live_runtime data;
static uint64_t fake_clock,fake_vdi,preempt_time,crumb_payload;
static unsigned delays,ack_at,preempt_calls,dispose_calls,crumb_value,inject_handoff,playback,checks,aux_frees,audio_polls,audio_wants,stream_ticks;
static int dispose_rc;
#define CHECK(c) do{assert(c);checks++;}while(0)
static int test_cas(uint32_t *p,uint32_t *expected,uint32_t desired,int weak,int success,int failure){if(inject_handoff&&p==&data.crumb_pad&&desired==0){*p=2;inject_handoff=0;}return __atomic_compare_exchange_n(p,expected,desired,weak,success,failure);}
#define __atomic_compare_exchange_n test_cas
static struct live_runtime *state(void){return &data;}
static uint64_t now_us(void){return fake_clock;}
static int live_playback_view(void){return playback;}
static void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b){(void)r;(void)t;(void)type;crumb_value=a;crumb_payload=b;}
static uint32_t live_stock_hold(struct live_runtime *,int,uint64_t);
static void live_stock_release_ack(struct live_runtime *);
static void free_aux(void *ctx,void *d){(void)ctx;assert(d==(void *)2);aux_frees++;}
static int32_t mock_delay(uint32_t n){fake_clock+=n*1000u;delays++;if(ack_at&&delays==ack_at){data.capture.descriptor=0;data.stream.fr_desc=0;data.stream.job=0;live_stock_release_ack(&data);}return 0;}
void live_encoder_preempt(live_encoder *e,uint32_t client){(void)e;assert(client!=4);preempt_calls++;assert(data.stock_handoff_lock==1);assert(data.crumb_pad==2||data.crumb_pad==3);if(data.crumb_pad==3)return;fake_clock+=preempt_time;data.capture.irq_timestamp=fake_clock;assert(live_stock_hold(&data,1,fake_clock)==2);}
int live_encoder_dispose(live_encoder *e){(void)e;dispose_calls++;assert(data.stock_handoff_lock==1&&data.crumb_pad==3);return dispose_rc;}
void live_stream_step(struct la_capture *c,struct live_stream *s,uint64_t n,int want,uint64_t (*clock)(void)){(void)n;(void)clock;stream_ticks++;if(!want&&s->job!=2&&s->job!=3){c->descriptor=0;s->job=0;}}
uint32_t live_stream_progress(const struct la_capture *c,const struct live_stream *s,uint64_t now){(void)c;(void)s;(void)now;return 0;}
static void live_audio_step(struct live_runtime *r,int want,uint64_t now){(void)now;if(want){audio_wants++;return;}if(!r->audio.active)return;if(++audio_polls==1){r->audio_z8.phase=LAZ8_STOPPING;return;}r->audio.active=0;r->audio.descriptor=0;r->audio_z8.phase=LAZ8_IDLE;}
static void reset(void){memset(&data,0,sizeof data);fake_clock=10000000;fake_vdi=preempt_time=delays=ack_at=preempt_calls=dispose_calls=crumb_value=crumb_payload=inject_handoff=playback=aux_frees=audio_polls=audio_wants=stream_ticks=dispose_rc=0;data.task_started=1;data.capture.ops.release=free_aux;}
'''
tests=r'''
int main(void){
 reset();live_w5_bringup_notify(4);CHECK(!data.crumb_pad&&!delays&&!preempt_calls&&!data.stock_handoff_lock);
 reset();data.capture.descriptor=(void *)1;preempt_time=2500000;ack_at=5;live_w5_bringup_notify(2);CHECK(data.crumb_pad==1&&delays==5&&data.vpad==12510&&crumb_value==3&&preempt_calls==2&&dispose_calls==1&&!data.stock_handoff_lock);
 reset();data.capture.descriptor=(void *)1;live_w5_bringup_notify(3);CHECK(delays==750&&data.crumb_pad==1&&crumb_value==4&&dispose_calls==0&&crumb_payload>>32==7);
 reset();ack_at=1;dispose_rc=-2;live_w5_bringup_notify(2);CHECK(crumb_value==4&&crumb_payload>>32==2&&dispose_calls==1&&!data.stock_handoff_lock);
 reset();playback=1;ack_at=1;live_w5_bringup_notify(2);CHECK(data.crumb_pad==1&&!data.stock_handoff_lock&&dispose_calls==1);
 worker_tick(1);CHECK(data.crumb_pad==1&&!audio_wants);
 ack_at=delays+1;live_w5_bringup_notify(3);CHECK(data.crumb_pad==1&&!data.stock_handoff_lock&&dispose_calls==2&&preempt_calls==4&&!audio_wants);
 reset();data.audio.active=1;data.audio.descriptor=(void *)3;data.audio_z8.phase=LAZ8_STOPPING;ack_at=1;live_w5_bringup_notify(2);CHECK(delays==750&&data.crumb_pad==1&&data.audio.active&&data.audio.descriptor&&dispose_calls==0&&crumb_value==4&&!data.stock_handoff_lock);
 reset();data.stock_handoff_lock=1;live_w5_bringup_notify(2);CHECK(crumb_value==6&&delays==3000&&!preempt_calls&&!dispose_calls&&data.stock_handoff_lock==1);
 reset();data.crumb_pad=2;data.vpad=10000;data.capture.irq_timestamp=11000000;CHECK(live_stock_hold(&data,1,13000000)==2);
 data.crumb_pad=3;CHECK(live_stock_hold(&data,1,13000000)==3);
 data.crumb_pad=1;fake_vdi=1;CHECK(live_stock_hold(&data,1,13000000)==1&&data.vpad==13000);
 fake_vdi=0;CHECK(live_stock_hold(&data,1,15000000)==1);
 data.capture.irq_timestamp=13200000;CHECK(live_stock_hold(&data,1,14000000)==1);
 CHECK(live_stock_hold(&data,0,15000000)==1);
 data.shutdown=1;CHECK(live_stock_hold(&data,1,15000000)==1);
 data.shutdown=0;CHECK(live_stock_hold(&data,1,15000000)==0);
 reset();data.crumb_pad=1;data.vpad=UINT32_MAX-500;data.capture.irq_timestamp=((uint64_t)UINT32_MAX+100)*1000;CHECK(live_stock_hold(&data,1,((uint64_t)UINT32_MAX+1500)*1000)==0);
 reset();data.crumb_pad=2;data.capture.descriptor=(void *)1;live_stock_release_ack(&data);CHECK(data.crumb_pad==2);
 data.capture.descriptor=0;data.stream.job=2;live_stock_release_ack(&data);CHECK(data.crumb_pad==2);
 data.stream.job=0;data.stream.fr_desc=(void *)2;live_stock_release_ack(&data);CHECK(data.crumb_pad==2);
 data.stream.fr_desc=0;data.audio.active=1;live_stock_release_ack(&data);CHECK(data.crumb_pad==2);
 data.audio.active=0;data.audio.descriptor=(void *)3;live_stock_release_ack(&data);CHECK(data.crumb_pad==2);
 data.audio.descriptor=0;data.audio_z8.phase=LAZ8_STOPPING;live_stock_release_ack(&data);CHECK(data.crumb_pad==2);
 data.audio_z8.phase=LAZ8_IDLE;live_stock_release_ack(&data);CHECK(data.crumb_pad==3);
 data.crumb_pad=1;live_stock_release_ack(&data);CHECK(data.crumb_pad==1);
 reset();data.crumb_pad=1;data.vpad=10000;data.capture.irq_timestamp=12000000;inject_handoff=1;CHECK(live_stock_hold(&data,1,14000000)==2&&data.crumb_pad==2);
 reset();playback=1;CHECK(live_stock_hold(&data,1,10000000)==1&&crumb_value==5);
 data.capture.irq_timestamp=20000000;CHECK(live_stock_hold(&data,1,20000000)==1&&data.vpad==20000);
 playback=0;CHECK(live_stock_hold(&data,1,22000000)==1);
 data.capture.irq_timestamp=21100000;CHECK(live_stock_hold(&data,1,22000000)==0);
 reset();data.crumb_pad=2;data.capture.descriptor=(void *)1;data.stream.fr_desc=(void *)2;data.audio.active=1;data.audio.descriptor=(void *)3;data.audio_z8.phase=LAZ8_RUNNING;data.requested=1;data.stream.tune.audio=1;data.capture.irq_timestamp=fake_clock;
 worker_tick(1);CHECK(!data.capture.descriptor&&!data.stream.fr_desc&&aux_frees==1&&data.audio.active&&data.crumb_pad==2&&!audio_wants);
 worker_tick(1);CHECK(!data.audio.active&&!data.audio.descriptor&&data.crumb_pad==3&&!audio_wants);
 reset();data.stream.fr_desc=(void *)2;data.stream.job=3;live_stream_release_aux(&data.capture,&data.stream);CHECK(data.stream.fr_desc&&!aux_frees);
 data.stream.job=0;data.capture.descriptor=(void *)1;live_stream_release_aux(&data.capture,&data.stream);CHECK(data.stream.fr_desc&&!aux_frees);
 data.capture.descriptor=0;live_stream_release_aux(&data.capture,&data.stream);live_stream_release_aux(&data.capture,&data.stream);CHECK(!data.stream.fr_desc&&aux_frees==1);
 printf("runtime handoff/worker: %u checks passed\n",checks);
}
'''
disposer=section(w,'int live_encoder_dispose','static int open_stream')
dispose_test=r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#define W5_MAGIC 0x57354c31u
#define W5_CLIENT 4u
struct live_w5 {uint32_t magic,close_failed,busy,release_req;void *arena_desc,*block_desc;};
typedef struct {void *persist,*scratch;unsigned frames;uint64_t last_pts;struct live_w5 *hw;} live_encoder;
static live_encoder encoder;
static unsigned native_alive,fake_client_map=255,used[2],frees,checks;
#define CLIENT_STREAM(c) fake_client_map
#define SLOT_USED(s) used[s]
#define CHECK(c) do{assert(c);checks++;}while(0)
static int alive(struct live_w5 *w){(void)w;return native_alive;}
static void pool_free(void *p){assert(!encoder.hw);if(++frees==1)assert(p==(void *)0x1234);else{assert(frees==2);free(p);}}
#define POOL_FREE(d) pool_free(d)
'''+disposer+r'''
int main(void){
 CHECK(live_encoder_dispose(0)==0);CHECK(live_encoder_dispose(&encoder)==0);
 struct live_w5 *w=calloc(1,sizeof(*w));assert(w);encoder.hw=w;
 CHECK(live_encoder_dispose(&encoder)==-1&&!frees);
 w->magic=W5_MAGIC;w->close_failed=1;CHECK(live_encoder_dispose(&encoder)==-2&&!frees);
 w->close_failed=0;w->busy=1;CHECK(live_encoder_dispose(&encoder)==-3&&!frees);
 w->busy=0;w->release_req=1;CHECK(live_encoder_dispose(&encoder)==-3&&!frees);
 w->release_req=0;native_alive=1;CHECK(live_encoder_dispose(&encoder)==-4&&!frees);
 native_alive=0;fake_client_map=1;used[1]=1;CHECK(live_encoder_dispose(&encoder)==-5&&!frees);
 fake_client_map=255;CHECK(live_encoder_dispose(&encoder)==-6&&!frees);
 w->arena_desc=w;w->block_desc=w;CHECK(live_encoder_dispose(&encoder)==-6&&!frees);
 w->arena_desc=(void *)0x1234;encoder.persist=(void *)1;encoder.scratch=(void *)2;encoder.frames=3;encoder.last_pts=4;
 CHECK(live_encoder_dispose(&encoder)==0&&frees==2&&!encoder.hw&&!encoder.persist&&!encoder.scratch&&!encoder.frames&&!encoder.last_pts);
 CHECK(live_encoder_dispose(&encoder)==0&&frees==2);
 printf("W5 safe pool disposal: %u checks passed\n",checks);
}
'''
sources={'runtime':preamble+handoff+aux+'static void worker_tick(int photo){struct live_runtime *r=&data;struct live_diag *D=&r->stream.diag;uint64_t w2=now_us();\n'+tick+'}\n'+tests,'dispose':dispose_test}
results=[]
with tempfile.TemporaryDirectory() as tmp:
 for name,body in sources.items():
  p=Path(tmp)/name;p.with_suffix('.c').write_text(body)
  cmd=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(S),str(p.with_suffix('.c')),'-o',str(p)]
  subprocess.run(cmd,check=True);q=subprocess.run([str(p)],check=True,capture_output=True,text=True)
  print(q.stdout,end='');results.append({'test':name,'result':q.stdout.strip(),'asan_ubsan':True})
(D/'handoff-tests.json').write_text(json.dumps({'tests':results,'source':'actual runtime handoff+worker fragment, stream aux release, and W5 dispose functions','hardware_proof':False},indent=2)+'\n')
