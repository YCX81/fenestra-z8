#!/usr/bin/env python3
"""编译执行同一 recorder C 的文件事务逻辑；仅替换硬件/原生入口。"""
import re,subprocess,sys
from pathlib import Path
h=Path(__file__).resolve().parent;b=Path(sys.argv[1]).resolve();out=b/'recorder-check'
s=(h/'recorder.c').read_text()
s=s.replace('#define F(a,t) ((t)(uintptr_t)UINT64_C(a))','#define F(a,t) ((t)test_fn(UINT64_C(a)))')
s='#include <stdlib.h>\n#include <assert.h>\n#include <string.h>\nstatic void *test_fn(unsigned long long);\n'+s
s=re.sub(r'#define R32\(a\).*', '#define R32(a) 0u',s);s=re.sub(r'#define R64\(a\).*','#define R64(a) 0ull',s)
a=s.index('static uint64_t ticks(');z=s.index('static void give(',a)
s=s[:a]+'''static uint64_t ticks(void){return 42;}
static int take(struct fr_state*s){if(s->lock)return 0;s->lock=1;return 1;}
'''+s[z:]
s=re.sub(r'static void copy32\([^\n]+','static void copy32(uint32_t *p,uint64_t at,unsigned n){(void)at;for(unsigned i=0;i<n;i++)p[i]=0;}',s)
s=s.replace('((volatile uint8_t*)(uintptr_t)UINT64_C(0xa02e98480))[i]','0xffu')
a=s.index('static const void *native_buffer(');z=s.index('/* RAM phases',a)
s=s[:a]+'static const void *native_buffer(struct fr_state*s,uint32_t n){(void)n;return s->io;}\n'+s[z:]
s+=r'''
static struct live_runtime *rt;
static unsigned char disk[FR_BYTES];
static int exists,lit,ops,shortwrite,badseek,closefail,openfail;
static unsigned pos,len,mode;
static uint32_t test_read32(void){return mode;}
struct fr_state *fr_context(void){struct fr_state*s=&rt->recorder;
 return s->arm && s->arm==s->generation?s:0;}
void fr_encoder_snapshot(uint32_t*p){memset(p,0,64);p[0]=0x57354c31;}
static void light(void*d,uint32_t n){(void)d;lit=n;}
static int32_t op(const char*p,uint32_t flags,uint32_t attr){
 assert(lit && !strncmp(p,"A:\\W5F",6) && attr==0x20);ops++;
 if(openfail)return -1;
 if(flags==0x101){if(exists)return -2;exists=1;len=0;}
 else assert(flags==1 && exists);
 pos=0;return 7;
}
static uint32_t sk(int32_t f,int64_t off,uint32_t origin,uint64_t*actual){
 assert(lit && f==7);ops++;pos=origin==2?len:(unsigned)off;*actual=pos+(badseek?1:0);return 1;}
static uint32_t wr(int32_t f,const void*p,uint32_t n,uint32_t*done,void*x){
 assert(lit && f==7 && !x && pos+n<=FR_BYTES);ops++;
 assert(rt->recorder.tx[rt->recorder.seq].phase==11);
 *done=shortwrite?n-1:n;memcpy(disk+pos,p,*done);pos+=*done;if(pos>len)len=pos;return 0;}
static uint32_t cl(int32_t f){assert(lit && f==7);ops++;
 assert(rt->recorder.tx[rt->recorder.seq].phase==13);return closefail?1:0;}
static void *test_fn(unsigned long long a){switch(a){
 case 0x40c15c3c:return (void*)light;case 0x402dd4f8:return (void*)op;
 case 0x402dd8e8:return (void*)sk;case 0x402dd78c:return (void*)wr;case 0x402dd558:return (void*)cl;
 default:abort();}}
static uint64_t movrs[128],adapter[160];
static uint64_t *beg=movrs+32,*svc=movrs+16,*waiter=movrs+30,*entry=adapter+64,*done=adapter+12;
static void reset(void){memset(rt,0,sizeof(*rt));memset(disk,0,sizeof(disk));
 exists=lit=ops=shortwrite=badseek=closefail=openfail=0;len=pos=mode=0;
 rt->capture.phase=LA_READY;}
static void prepare(void){rt->recorder.arm=1;fr_prepare(rt);assert(rt->recorder.ready);mode=1;}
static void begin_test(void){memset(movrs,0,sizeof(movrs));beg[30]=0x40d07ba8;beg[0]=0;fr_probe(1,beg);}
static void service(void){svc[19]=0;svc[30]=0x402f303c;svc[47]=0x40d07ba8;fr_probe(2,svc);}
static void adapter_start(void){beg[30]=0x40d07bb0;fr_probe(4,beg);entry[0]=1;fr_probe(14,entry);}
static void *wait_event(void*x){(void)x;waiter[30]=0x402f58cc;waiter[33]=0x40d07bb0;fr_probe(5,waiter);return 0;}
static void *done_event(void*x){(void)x;done[19]=1;done[30]=0x402f4c1c;fr_probe(13,done);return 0;}
int main(void){rt=calloc(1,sizeof(*rt));assert(rt);
 reset();for(int i=0;i<20;i++){fr_prepare(rt);begin_test();}assert(ops==0);
 prepare();assert(len==FR_BYTES && ops==3 && !lit);int n=ops;fr_prepare(rt);assert(ops==n);
 begin_test();assert(rt->recorder.seq==2);service();assert(rt->recorder.seq==3);
 adapter_start();done_event(0);assert(!(rt->recorder.seen&FR_DONE));wait_event(0);
 assert(rt->recorder.seen&FR_DONE);assert(rt->recorder.seq==4 && ops==18);
 n=ops;begin_test();service();done_event(0);assert(ops==n);
 reset();prepare();rt->recorder.lock=1;n=ops;begin_test();
 assert((rt->recorder.seen&2) && (rt->recorder.missed&2) && ops==n);
 rt->recorder.lock=0;
 /* Wrong client, stack, LR, photo, generation never write shared service. */
 reset();prepare();begin_test();n=ops;svc[30]=0x402f303c;svc[47]=0x40d07ba8;
 svc[19]=4;fr_probe(2,svc);assert(ops==n);svc[19]=0;svc[47]=1;fr_probe(2,svc);assert(ops==n);
 svc[47]=0x40d07ba8;mode=0;fr_probe(2,svc);assert(ops==n);mode=1;
 rt->recorder.arm=2;fr_probe(2,svc);assert(ops==n);
 /* Merged callback -> next REC cannot consume the previous generation.
  * Exercise the actual outer predicate, lock busy and ready=0 too. */
 for(int busy=0;busy<2;busy++)for(int photo=0;photo<2;photo++){
  reset();prepare();begin_test();service();beg[30]=0x40d07bb0;fr_probe(4,beg);
  assert(rt->recorder.pending==1);rt->recorder.lock=busy;
  if(busy)rt->recorder.ready=0;
  n=ops;mode=photo?0:1;entry[0]=3;
  assert(fr_enabled(14,entry));fr_probe(14,entry);
  assert(!rt->recorder.pending && !rt->recorder.adapter_gen && !rt->recorder.ready);
  assert(rt->recorder.seen&FR_DONE);assert(rt->recorder.claimed);
  mode=1;wait_event(0);rt->recorder.lock=0;
  uint64_t session=rt->recorder.session;unsigned seq=rt->recorder.seq;
  begin_test();service();adapter_start();done_event(0);wait_event(0);
  assert(ops==n && rt->recorder.seq==seq && rt->recorder.session==session);
  assert(!rt->recorder.pending && !rt->recorder.adapter_gen);
  assert(!(rt->recorder.seen&(1u<<13)));
 }
 /* Both orders and real simultaneous arrivals while write lock is held.
  * Also prove ready=0 does not prevent lifecycle completion. */
 for(int i=0;i<200;i++){
  reset();prepare();begin_test();adapter_start();rt->recorder.lock=1;rt->recorder.ready=0;n=ops;
  pthread_t a,b;assert(!pthread_create(&a,0,wait_event,0));assert(!pthread_create(&b,0,done_event,0));
  pthread_join(a,0);pthread_join(b,0);
  assert((rt->recorder.seen&FR_DONE) && (rt->recorder.seen&(1u<<13)) && (rt->recorder.seen&(1u<<5)));
  assert(rt->recorder.missed&(1u<<13));assert(ops==n);
 }
 reset();exists=1;disk[10]=123;rt->recorder.arm=1;fr_prepare(rt);assert(!rt->recorder.ready && disk[10]==123);
 n=ops;fr_prepare(rt);assert(ops==n);exists=0;rt->recorder.arm=2;fr_prepare(rt);assert(rt->recorder.ready);
 reset();openfail=1;rt->recorder.arm=1;fr_prepare(rt);assert(!rt->recorder.ready);n=ops;fr_prepare(rt);assert(ops==n);
 openfail=0;rt->recorder.arm=2;fr_prepare(rt);assert(rt->recorder.ready);
 reset();prepare();badseek=1;begin_test();assert(!rt->recorder.ready && rt->recorder.tx[1].phase==18);
 reset();prepare();shortwrite=1;begin_test();assert(!rt->recorder.ready && rt->recorder.tx[1].error==0xf003);
 reset();prepare();closefail=1;begin_test();assert(!rt->recorder.ready && rt->recorder.tx[1].close_rc==1);
 free(rt);return 0;}
'''
s=s.replace('#define R32(a) 0u','#define R32(a) test_read32()')
s='#include <pthread.h>\nstatic uint32_t test_read32(void);\n'+s
s=s.replace('static uint32_t test_read32(void);','static unsigned test_read32(void);')
target=b/'recorder/host_io_test.c';target.write_text(s)
subprocess.run(['clang','-U__aarch64__','-std=c11','-pthread','-Wall','-Wextra','-Werror','-I',str(b/'ui/core/include'),str(target),'-o',str(out/'host_io_test')],check=True)
subprocess.run([str(out/'host_io_test')],check=True)
print('PASS: arm isolation, explicit retries, old evidence preserved, identity rejection, merged callback -> next REC isolation, 3 REC writes, concurrent end with lock busy and ready=0, native return failures')
print('LIMIT: stubs replace native I/O, stock RAM and write-lock LL/SC; lifecycle atomics run on host, not AArch64 hardware')
