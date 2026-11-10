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
static int take(struct fr_state*s){unsigned z=0;return __atomic_compare_exchange_n(&s->lock,&z,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
'''+s[z:]
s=re.sub(r'static void copy32\([^\n]+','static void copy32(uint32_t *p,uint64_t at,unsigned n){(void)at;for(unsigned i=0;i<n;i++)p[i]=0;}',s)
s=s.replace('((volatile uint8_t*)(uintptr_t)UINT64_C(0xa02e98480))[i]','0xffu')
a=s.index('static const void *native_buffer(');z=s.index('/* RAM phases',a)
s=s[:a]+'static const void *native_buffer(struct fr_state*s,uint32_t n){(void)n;return s->io;}\n'+s[z:]
s+=r'''
static struct live_runtime *rt;
static unsigned char disk[FR_BYTES];
static int exists,lit,ops,shortwrite,badseek,closefail,openfail,writefail,invalid,snapshots,claim_race;
static unsigned pos,len,mode;
static uint32_t test_read32(void){return mode;}
struct fr_state *fr_context(void){struct fr_state*s=&rt->recorder;
 return !invalid && s->arm?s:0;}
void fr_encoder_snapshot(uint32_t*p){snapshots++;memset(p,0,64);p[0]=0x57354c31;}
static void light(void*d,uint32_t n){(void)d;lit=n;}
static int32_t op(const char*p,uint32_t flags,uint32_t attr){
 assert(lit && !strncmp(p,"A:\\W5F",6) && attr==0x20);ops++;
 if(openfail)return -1;
 if(flags==0x101){if(exists)return -2;exists=1;len=0;}
 else assert(flags==1 && exists);
 pos=0;return 7;
}
static uint32_t sk(int32_t f,int64_t off,uint32_t origin,uint64_t*actual){
 assert(lit && f==7);ops++;pos=origin==2?len:(unsigned)off;*actual=pos+((badseek==1 || (badseek==2 && origin==0))?1:0);return 1;}
static uint32_t wr(int32_t f,const void*p,uint32_t n,uint32_t*done,void*x){
 assert(lit && f==7 && !x && pos+n<=FR_BYTES);ops++;
 assert(rt->recorder.tx[rt->recorder.seq].phase==11);
 *done=shortwrite?n-1:n;memcpy(disk+pos,p,*done);pos+=*done;if(pos>len)len=pos;return writefail?0x1234:0;}
static uint32_t cl(int32_t f){assert(lit && f==7);ops++;
 assert(rt->recorder.tx[rt->recorder.seq].phase==13);return closefail?1:0;}
static void *test_fn(unsigned long long a){switch(a){
 case 0x40c15c3c:return (void*)light;case 0x402dd4f8:return (void*)op;
 case 0x402dd8e8:return (void*)sk;case 0x402dd78c:return (void*)wr;case 0x402dd558:return (void*)cl;
 default:abort();}}
static uint64_t movrs[128],adapter[160];
static uint64_t *beg=movrs+32,*svc=movrs+16,*waiter=movrs+30,*entry=adapter+64,*done=adapter+12;
static void reset(void){memset(rt,0,sizeof(*rt));memset(disk,0,sizeof(disk));
 exists=lit=ops=shortwrite=badseek=closefail=openfail=writefail=invalid=snapshots=claim_race=0;len=pos=mode=0;
 rt->capture.phase=LA_READY;}
static void prepare(void){rt->recorder.arm=1;fr_prepare(rt);assert(rt->recorder.ready && rt->recorder.file_owned);mode=1;}
static void setup_regs(void){memset(movrs,0,sizeof(movrs));beg[30]=0x40d07ba8;beg[0]=0;}
static void before_test(void){setup_regs();fr_before(beg);}
static void begin_test(void){memset(movrs,0,sizeof(movrs));beg[30]=0x40d07ba8;beg[0]=0;fr_probe(1,beg);}
static void service(void){svc[19]=0;svc[30]=0x402f303c;svc[47]=0x40d07ba8;fr_probe(2,svc);}
static void adapter_start(void){beg[30]=0x40d07bb0;fr_probe(4,beg);entry[0]=1;fr_probe(14,entry);}
static void *wait_event(void*x){(void)x;waiter[30]=0x402f58cc;waiter[33]=0x40d07bb0;fr_probe(5,waiter);return 0;}
static void *done_event(void*x){(void)x;done[19]=1;done[30]=0x402f4c1c;fr_probe(13,done);return 0;}
static void *before_thread(void*x){(void)x;fr_before(beg);return 0;}
int main(void){rt=calloc(1,sizeof(*rt));assert(rt);
 reset();for(int i=0;i<20;i++){fr_prepare(rt);begin_test();}assert(ops==0);
 prepare();assert(len==FR_BYTES && ops==3 && !lit);int n=ops;fr_prepare(rt);assert(ops==n);
 before_test();assert(rt->recorder.seq==1 && !snapshots);
 begin_test();assert(rt->recorder.seq==2);service();assert(rt->recorder.seq==4);
 adapter_start();done_event(0);assert(!(rt->recorder.seen&FR_DONE));wait_event(0);
 assert(rt->recorder.seen&FR_DONE);assert(rt->recorder.seq==5 && ops==23);
 n=ops;begin_test();assert(ops==n+5);n=ops;begin_test();service();done_event(0);assert(ops==n);
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
  assert(ops==n+5 && rt->recorder.seq==3 && rt->recorder.session==session);
  (void)seq;
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
 reset();prepare();badseek=1;begin_test();assert(!rt->recorder.ready && rt->recorder.tx[2].phase==18);
 reset();prepare();shortwrite=1;begin_test();assert(!rt->recorder.ready && rt->recorder.tx[2].error==0xf003);
 reset();prepare();closefail=1;begin_test();assert(!rt->recorder.ready && rt->recorder.tx[2].close_rc==1);

 /* Any client (including high raw X0) reaches entry and post hand-back. */
 for(unsigned i=0;i<4;i++){
  static const uint64_t clients[]={0,2,4,UINT64_C(0x12345678ffffffff)};
  reset();prepare();setup_regs();beg[0]=clients[i];fr_before(beg);
  assert(!snapshots && rt->recorder.tx[1].phase==16);
  assert(fr_enabled(1,beg));fr_probe(1,beg);
  uint32_t *p=(void*)(disk+2*512);assert(p[3]==1 && p[10]==(uint32_t)clients[i]);
  assert(((uint64_t)p[13]<<32|p[12])==clients[i]);
  svc[19]=(uint32_t)clients[i];svc[30]=0x402f303c;svc[47]=0x40d07ba8;
  fr_probe(2,svc);assert(rt->recorder.tx[4].phase==16);
 }
 /* Exact LR and valid armed runtime are mandatory even for diagnostics. */
 reset();prepare();setup_regs();beg[30]++;n=ops;fr_before(beg);fr_probe(1,beg);assert(ops==n);
 beg[30]--;invalid=1;fr_before(beg);fr_probe(1,beg);assert(ops==n);
 invalid=0;rt->recorder.arm=0;fr_before(beg);fr_probe(1,beg);assert(ops==n);
 /* Each gate yields a reason at the fixed rejection sector, once per boot.
  * Epoch is the only nonblocking reason. Entry always precedes association. */
 for(unsigned mask=1;mask<64;mask++){
  reset();prepare();setup_regs();
  if(mask&FR_EPOCH)rt->card_epoch++;
  if(mask&FR_SHUTDOWN)rt->shutdown=1;
  if(mask&FR_ARM)rt->recorder.arm=2;
  if(mask&FR_NOT_READY)rt->recorder.ready=0;
  if(mask&FR_CLAIMED)rt->recorder.claimed=1;
  if(mask&FR_MODE)mode=0;
  n=ops;fr_before(beg);assert(ops==n+10 && !snapshots);
  uint32_t *p=(void*)(disk+3*512);assert(p[3]==FR_REJECT && p[125]==mask);
  assert(p[126]==rt->card_epoch && p[5]==rt->shutdown);
  assert(fr_enabled(1,beg));n=ops;fr_probe(1,beg);
  assert(ops==n+(mask==FR_EPOCH?5:0));
  if(mask==FR_EPOCH){assert(rt->recorder.seen&2);service();assert(rt->recorder.tx[4].phase==16);}
  n=ops;fr_probe(1,beg);fr_before(beg);assert(ops==n);
 }
 /* Failure at entry never retries entry; returned failure can be reported
  * by the post-hand-back rejection. The harness models return, not hang. */
 for(int fault=0;fault<6;fault++){
  reset();prepare();setup_regs();
  openfail=fault==0;badseek=fault==1?1:fault==5?2:0;shortwrite=fault==2;writefail=fault==3;closefail=fault==4;
  fr_before(beg);assert(!rt->recorder.ready && rt->recorder.tx[1].phase==18);
  n=ops;openfail=badseek=shortwrite=writefail=closefail=0;
  fr_probe(1,beg);assert(ops==n+5 && rt->recorder.tx[3].phase==16);
  uint32_t *p=(void*)(disk+3*512);assert((p[125]&FR_NOT_READY) && p[120]==18);
  n=ops;fr_before(beg);assert(ops==n);
 }
 /* Two simultaneous exact-LR entries/rejections: at most one each;
  * no overwrite of immutable prepared sector or accepted REC identity. */
 for(int i=0;i<200;i++){
  reset();prepare();setup_regs();rt->shutdown=1;
  pthread_t a,b;assert(!pthread_create(&a,0,before_thread,0));assert(!pthread_create(&b,0,before_thread,0));
  pthread_join(a,0);pthread_join(b,0);
  assert(rt->recorder.entry_claimed==1 && rt->recorder.reject_claimed==1);
  assert(ops>=3 && ops<=13 && !snapshots && !rt->recorder.claimed);
  assert(((uint32_t*)(void*)disk)[3]==0);
 }
 /* Failed exclusive create must not authorize writes to old evidence,
  * even though entry/reject bypass ready. No prepared generation is similar. */
 reset();exists=1;len=FR_BYTES;memset(disk,0x5a,sizeof(disk));rt->recorder.arm=1;fr_prepare(rt);
 assert(!rt->recorder.file_owned);mode=1;n=ops;before_test();begin_test();assert(ops==n);
 for(unsigned i=0;i<FR_BYTES;i++)assert(disk[i]==0x5a);
 reset();rt->recorder.arm=1;mode=1;before_test();begin_test();assert(!ops);
 /* Preparation failure never authorizes REC I/O, even when WRITE filled the
  * entire file and only CLOSE failed. Clear faults before REC so neither EOF
  * nor a continuing stub failure can hide an unauthorized later transaction. */
 for(int fault=3;fault>=0;fault--){
  reset();rt->recorder.arm=1;
  openfail=fault==0;shortwrite=fault==1;writefail=fault==2;closefail=fault==3;
  fr_prepare(rt);
  assert(!rt->recorder.ready && !rt->recorder.file_owned && rt->recorder.tx[0].phase==18);
  if(fault==3){
   assert(exists && len==FR_BYTES && ops==3 && !lit);
   assert(rt->recorder.tx[0].written==FR_BYTES && !rt->recorder.tx[0].error && rt->recorder.tx[0].close_rc==1);
  }
  unsigned char saved[FR_BYTES];memcpy(saved,disk,sizeof(saved));
  n=ops;openfail=shortwrite=writefail=closefail=0;mode=1;
  before_test();assert(ops==n && rt->recorder.entry_claimed && rt->recorder.reject_claimed);
  assert(rt->recorder.tx[1].error==0xf004 && rt->recorder.tx[3].error==0xf004);
  begin_test();service();adapter_start();done_event(0);wait_event(0);
  before_test();begin_test();fr_prepare(rt);
  assert(ops==n && !memcmp(saved,disk,sizeof(saved)));
  assert(!rt->recorder.ready && !rt->recorder.file_owned);
 }
 /* CAS succeeds, then a mutable gate changes before identity publication. */
 reset();prepare();setup_regs();claim_race=1;fr_probe(1,beg);
 assert(rt->recorder.claimed && !(rt->recorder.seen&2));
 assert(((uint32_t*)(void*)(disk+1536))[125]==(FR_SHUTDOWN|FR_RACE|FR_AFTER));
 /* Dump actual C-produced sectors for the Python decoder integration. */
 reset();prepare();before_test();begin_test();service();adapter_start();done_event(0);wait_event(0);begin_test();
 FILE *f=fopen("recorder-v3-fixture.bin","wb");assert(f);assert(fwrite(disk,1,FR_BYTES,f)==FR_BYTES);assert(!fclose(f));
 free(rt);return 0;}
'''
s=s.replace('why=reasons(s)&~FR_CLAIMED;', 'test_after_claim();why=reasons(s)&~FR_CLAIMED;')
s=s.replace('static uint32_t test_read32(void){return mode;}', 'static void test_after_claim(void){if(claim_race)rt->shutdown=1;}\nstatic uint32_t test_read32(void){return mode;}')
s='static void test_after_claim(void);\n'+s
s=s.replace('#define R32(a) 0u','#define R32(a) test_read32()')
s='#include <stdio.h>\n#include <pthread.h>\nstatic uint32_t test_read32(void);\n'+s
s=s.replace('static uint32_t test_read32(void);','static unsigned test_read32(void);')
target=b/'recorder/host_io_test.c';target.write_text(s)
subprocess.run(['clang','-U__aarch64__','-std=c11','-pthread','-Wall','-Wextra','-Werror','-I',str(b/'ui/core/include'),str(target),'-o',str(out/'host_io_test')],check=True)
subprocess.run([str(out/'host_io_test')],check=True,cwd=out)
import decoder
rows,errors=decoder.decode((out/'recorder-v3-fixture.bin').read_bytes())
assert not errors and [r['事件号'] for r in rows]==list(decoder.EVENTS),(rows,errors)
print('PASS: arm isolation, explicit retries, old evidence preserved, preparation OPEN/short WRITE/WRITE error/CLOSE failure -> zero subsequent REC I/O, identity rejection, merged callback -> next REC isolation, six fixed sectors, all clients, 63 gate combinations, epoch nonblocking, pre-entry failures, 200 concurrent entries/rejections, concurrent end with lock busy and ready=0, native return failures')
print('LIMIT: stubs replace native I/O, stock RAM and write-lock LL/SC; lifecycle atomics run on host, not AArch64 hardware')
