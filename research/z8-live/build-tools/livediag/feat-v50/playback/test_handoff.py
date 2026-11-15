#!/usr/bin/env python3
"""Execute the actual runtime hold functions with only hardware/time calls stubbed."""
from pathlib import Path
import subprocess,tempfile,json,sys
D=Path(__file__).resolve().parent
src=Path(sys.argv[1]) if len(sys.argv)>1 else D/'runtime.c'
s=src.read_text();a=s.index('void live_w5_bringup_notify');b=s.index('/* DMA callback context',a)
body=s[a:b].replace('*(volatile uint64_t *)(uintptr_t)UINT64_C(0xa02d3d980)','fake_vdi')
preamble=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define FN(a,t) mock_delay
#define EV_W5 1
#define EV_MODE 2
struct live_runtime {uint32_t crumb_pad,vpad,task_started,shutdown;struct {uintptr_t descriptor;uint64_t irq_timestamp;} capture;struct {uint32_t job;int encoder;} stream;};
static struct live_runtime data;
static uint64_t fake_clock,fake_vdi,preempt_time;
static unsigned delays,ack_at,preempt_calls,crumb_type,crumb_value,inject_handoff;
static int test_cas(uint32_t *p,uint32_t *expected,uint32_t desired,int weak,int success,int failure){if(inject_handoff&&desired==0){*p=2;inject_handoff=0;}return __atomic_compare_exchange_n(p,expected,desired,weak,success,failure);}
#define __atomic_compare_exchange_n test_cas
static struct live_runtime *state(void){return &data;}
static uint64_t now_us(void){return fake_clock;}
static void crumb(struct live_runtime *r,uint64_t t,uint32_t type,uint32_t a,uint64_t b){(void)r;(void)t;(void)b;crumb_type=type;crumb_value=a;}
static uint32_t live_stock_hold(struct live_runtime *,int,uint64_t);
static void live_stock_release_ack(struct live_runtime *);
static int32_t mock_delay(uint32_t n){fake_clock+=n*1000u;delays++;if(ack_at&&delays==ack_at){data.capture.descriptor=0;data.stream.job=0;live_stock_release_ack(&data);}return 0;}
static void live_encoder_preempt(int *e,uint32_t client){(void)e;preempt_calls++;if(client!=4){assert(data.crumb_pad==2||data.crumb_pad==3);if(data.crumb_pad==3)return;fake_clock+=preempt_time;data.capture.irq_timestamp=fake_clock;assert(live_stock_hold(&data,1,fake_clock)==2);}}
static void reset(void){memset(&data,0,sizeof data);fake_clock=10000000;fake_vdi=preempt_time=delays=ack_at=preempt_calls=crumb_type=crumb_value=inject_handoff=0;data.task_started=1;}
'''
tests=r'''
int main(void){
 reset();live_w5_bringup_notify(4);assert(!data.crumb_pad&&!delays&&preempt_calls==1);
 reset();data.capture.descriptor=1;preempt_time=2500000;ack_at=5;live_w5_bringup_notify(2);assert(data.crumb_pad==1&&delays==5&&data.vpad==12510&&crumb_value==3&&preempt_calls==2);
 reset();data.capture.descriptor=1;live_w5_bringup_notify(3);assert(delays==750&&data.crumb_pad==1&&data.vpad==11500&&crumb_value==4);
 reset();data.crumb_pad=2;data.vpad=10000;data.capture.irq_timestamp=11000000;assert(live_stock_hold(&data,1,13000000)==2);
 data.crumb_pad=3;assert(live_stock_hold(&data,1,13000000)==3);
 data.crumb_pad=1;fake_vdi=1;assert(live_stock_hold(&data,1,13000000)==1&&data.vpad==13000);
 fake_vdi=0;assert(live_stock_hold(&data,1,15000000)==1); /* old IRQ from before stock teardown */
 data.capture.irq_timestamp=13200000;assert(live_stock_hold(&data,1,14000000)==1); /* exactly 1 s */
 assert(live_stock_hold(&data,0,15000000)==1); /* movie mode */
 data.shutdown=1;assert(live_stock_hold(&data,1,15000000)==1);
 data.shutdown=0;assert(live_stock_hold(&data,1,15000000)==0); /* slow LV, no freshness requirement */
 reset();data.crumb_pad=1;data.vpad=UINT32_MAX-500;data.capture.irq_timestamp=((uint64_t)UINT32_MAX+100)*1000;assert(live_stock_hold(&data,1,((uint64_t)UINT32_MAX+1500)*1000)==0);
 reset();data.crumb_pad=2;data.capture.descriptor=1;live_stock_release_ack(&data);assert(data.crumb_pad==2);
 data.capture.descriptor=0;data.stream.job=2;live_stock_release_ack(&data);assert(data.crumb_pad==2);
 data.stream.job=0;live_stock_release_ack(&data);assert(data.crumb_pad==3);
 data.crumb_pad=1;live_stock_release_ack(&data);assert(data.crumb_pad==1);
 reset();data.crumb_pad=1;data.vpad=10000;data.capture.irq_timestamp=12000000;inject_handoff=1;assert(live_stock_hold(&data,1,14000000)==2&&data.crumb_pad==2);
 puts("handoff: 17 state and ordering checks passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(preamble+body+tests)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 result=subprocess.run([str(p/'test')],check=True,text=True,capture_output=True)
 print(result.stdout,end='')
