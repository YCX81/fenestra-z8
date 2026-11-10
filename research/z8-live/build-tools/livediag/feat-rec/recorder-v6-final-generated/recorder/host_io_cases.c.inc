static struct live_runtime *rt;
static unsigned char disk[FR_BYTES], maps[5];
static int exists,lit,ops,shortwrite,badseek,closefail,openfail,writefail,invalid,forbid_io;
static unsigned pos,len,mode,can_sleep,delayfail,mapreads,delays,edges,quantum;
static uint64_t clock_us,freq;
static unsigned expect_map_before_light;
static uint64_t cancel_at;
static FILE *wave;
static unsigned test_mode(void){return mode;}
static unsigned test_sleepable(void){return can_sleep;}
static uint64_t test_clock(void){return clock_us;}
static uint64_t test_frequency(void){return freq;}
static unsigned test_map(unsigned client){assert(client<5);mapreads++;return maps[client];}
struct fr_state *fr_context(void){return !invalid && rt->recorder.arm?&rt->recorder:0;}
static void light(void *gpio,uint32_t on){
 assert((uintptr_t)gpio==UINT64_C(0xa0280a1b0));lit=on;edges++;
 if(expect_map_before_light)assert(mapreads==1);
 if(wave)fprintf(wave,"%llu,%u\n",(unsigned long long)clock_us,on);
}
static int32_t delay(uint32_t n){
 assert(n==1 && can_sleep);delays++;
 if(delayfail)return -25;
 clock_us+=quantum;if(cancel_at && clock_us>=cancel_at)__atomic_fetch_or(&rt->recorder.missed,FR_RETURNED,__ATOMIC_RELEASE);return 0;
}
static int32_t op(const char *p,uint32_t flags,uint32_t attr){
 assert(!forbid_io && lit && !strncmp(p,"A:\\W5L",6) && attr==0x20);ops++;
 if(openfail)return -1;
 if(flags==0x101){if(exists)return -2;exists=1;len=0;}
 else assert(flags==1 && exists);
 pos=0;return 7;
}
static uint32_t sk(int32_t fd,int64_t off,uint32_t origin,uint64_t *actual){
 assert(!forbid_io && lit && fd==7);ops++;pos=origin==2?len:(unsigned)off;
 *actual=pos+((badseek==1 || (badseek==2 && origin==0))?1:0);return 1;
}
static uint32_t wr(int32_t fd,const void *p,uint32_t n,uint32_t *done,void *x){
 assert(!forbid_io && lit && fd==7 && !x && pos+n<=FR_BYTES);ops++;
 assert(rt->recorder.tx[rt->recorder.seq].phase==11);
 *done=shortwrite?n-1:n;memcpy(disk+pos,p,*done);pos+=*done;if(pos>len)len=pos;
 return writefail?0x1234:0;
}
static uint32_t cl(int32_t fd){assert(!forbid_io && lit && fd==7);ops++;
 assert(rt->recorder.tx[rt->recorder.seq].phase==13);return closefail?1:0;}
static void *test_fn(unsigned long long address){switch(address){
 case 0x40c15c3c:return (void*)light;case 0x40187b00:return (void*)delay;
 case 0x402dd4f8:return (void*)op;case 0x402dd8e8:return (void*)sk;
 case 0x402dd78c:return (void*)wr;case 0x402dd558:return (void*)cl;
 default:abort();}}
static uint64_t movrs[128], foreign[128];
static uint64_t *beg=movrs+32,*svc=movrs+16;
static void reset(void){
 memset(rt,0,sizeof(*rt));memset(disk,0,sizeof(disk));memset(movrs,0,sizeof(movrs));memset(maps,255,5);
 exists=lit=ops=shortwrite=badseek=closefail=openfail=writefail=invalid=forbid_io=0;
 pos=len=mode=delayfail=mapreads=delays=edges=expect_map_before_light=0;clock_us=0;cancel_at=0;can_sleep=1;quantum=1000;freq=1000000;
 beg[30]=UINT64_C(0x40d07ba8);beg[0]=0;rt->capture.phase=LA_READY;
}
static void prepare(void){rt->recorder.arm=1;fr_prepare(rt);assert(rt->recorder.ready && rt->recorder.file_owned);mode=1;}
static struct fr_state *before(void){return fr_before(beg);}
static void ir(struct fr_state *s){fr_light(s,FR_I,beg);fr_light(s,FR_R,beg);}
static void service(uint32_t value){svc[0]=UINT64_C(0x1234567800000000)|value;svc[19]=rt->recorder.client;svc[30]=0x402f303c;svc[47]=0x40d07ba8;fr_probe(FR_S,svc);}
static void mapping(unsigned value){maps[0]=value;svc[19]=rt->recorder.client;svc[30]=0x402f3068;svc[47]=0x40d07ba8;fr_probe(FR_M,svc);}
static void complete(struct fr_state *s){ir(s);service(0);mapping(1);}
static void *entry_thread(void *p){*(struct fr_state **)p=before();return 0;}
int main(void){
 rt=calloc(1,sizeof(*rt));assert(rt);
 reset();for(int i=0;i<10;i++){fr_prepare(rt);assert(!before());fr_probe(FR_S,svc);}assert(!ops && !edges);
 reset();prepare();assert(ops==3 && len==FR_BYTES && !lit);
 forbid_io=1;struct fr_state *s=before();assert(s && ops==3 && s->tx[1].phase==0);
 forbid_io=1;complete(s);assert(ops==3 && s->lamp_seen==15 && !s->lamp_failed && (s->seen&FR_DONE));
 assert(clock_us==6000000 && !lit && delays==6000 && mapreads==1);
 unsigned oldedges=edges;assert(!before());complete(s);fr_prepare(rt);assert(edges==oldedges);
 for(unsigned i=1024;i<FR_BYTES;i++)assert(!disk[i]);
 FILE *f=fopen("recorder-v4-fixture.bin","wb");assert(f);assert(fwrite(disk,1,FR_BYTES,f)==FR_BYTES);assert(!fclose(f));
 /* Wrong full LR (including high 32 bits), runtime, arm never consume boot. */
 reset();prepare();beg[30]=UINT64_C(0x140d07ba8);assert(!before());beg[30]=0x40d07ba8;
 invalid=1;assert(!before());invalid=0;rt->recorder.arm=0;assert(!before());assert(!rt->recorder.entry_claimed && ops==3);
 /* No readiness, mode, shutdown, epoch, generation or file requirement for lamps. */
 for(unsigned mask=0;mask<32;mask++){
  reset();prepare();
  if(mask&1)rt->recorder.ready=0;
  if(mask&2)rt->shutdown=1;
  if(mask&4)rt->card_epoch++;
  if(mask&8)rt->recorder.arm=2;
  if(mask&16)mode=0;
  s=before();assert(s);forbid_io=1;complete(s);assert(s->lamp_seen==15 && !s->lamp_failed && ops==3);
 }
 /* Pinned I/R continue if arm/runtime lookup is lost after entry. */
 reset();prepare();s=before();forbid_io=1;rt->recorder.arm=0;invalid=1;ir(s);
 assert(s->lamp_seen==3 && clock_us==3000000);fr_probe(FR_S,svc);assert(s->lamp_seen==3);
 /* Skip S/M under arm=0, then reuse the same SP/LR/client on next REC. */
 reset();prepare();s=before();forbid_io=1;ir(s);rt->recorder.arm=0;
 service(0);mapping(1);assert(s->lamp_seen==3 && !(s->seen&FR_DONE));
 rt->recorder.arm=2;assert(!before());oldedges=edges;service(0);mapping(1);
 assert(s->lamp_seen==3 && (s->seen&FR_DONE) && edges==oldedges);
 /* Native entry errors: no retry/rejection I/O; all four codes remain possible. */
 for(int fault=0;fault<6;fault++){
  reset();prepare();openfail=fault==0;badseek=fault==1?1:fault==5?2:0;
  shortwrite=fault==2;writefail=fault==3;closefail=fault==4;
  s=before();assert(s && s->ready && s->tx[1].phase==0);int n=ops;
  forbid_io=1;complete(s);assert(ops==n && s->lamp_seen==15);assert(!before());fr_prepare(rt);
 }
 /* Preparation failure cannot authorize writes to existing/partial files. */
 for(int fault=0;fault<5;fault++){
  reset();rt->recorder.arm=1;openfail=fault==0;shortwrite=fault==1;writefail=fault==2;closefail=fault==3;
  if(fault==4){exists=1;len=FR_BYTES;memset(disk,0x5a,sizeof(disk));}
  fr_prepare(rt);assert(!rt->recorder.file_owned);int n=ops;
  unsigned char copy[FR_BYTES];memcpy(copy,disk,sizeof(copy));
  openfail=shortwrite=writefail=closefail=0;forbid_io=1;s=before();assert(s && s->tx[1].error==0);
  complete(s);fr_prepare(rt);assert(ops==n && !memcmp(copy,disk,sizeof(copy)));
 }
 reset();rt->recorder.arm=1;s=before();forbid_io=1;complete(s);assert(!ops && s->lamp_seen==15);
 /* Only pre-entry retry with a larger token is allowed. */
 reset();rt->recorder.arm=1;openfail=1;fr_prepare(rt);int n=ops;fr_prepare(rt);assert(ops==n);
 openfail=0;rt->recorder.arm=2;fr_prepare(rt);assert(rt->recorder.ready);s=before();
 forbid_io=1;rt->recorder.arm=3;fr_prepare(rt);assert(rt->recorder.generation==2);
 /* Lock contention at entry still publishes identity without a file operation. */
 reset();prepare();rt->recorder.lock=1;s=before();assert(s && !(s->missed&(1u<<15)) && ops==3);
 forbid_io=1;complete(s);assert(s->lamp_seen==15);
 /* SP first; then full stack LR, W19 and S's full current LR. */
 reset();prepare();s=before();forbid_io=1;service(3);assert(s->lamp_seen==4);
 reset();prepare();s=before();forbid_io=1;memcpy(foreign,movrs,sizeof(movrs));
 fr_probe(FR_S,foreign+16);assert(!s->lamp_seen);
 svc[30]=0x402f303c;svc[19]=0;svc[47]=UINT64_C(0x140d07ba8);fr_probe(FR_S,svc);assert(!s->lamp_seen);
 svc[47]=0x40d07ba8;svc[19]=1;fr_probe(FR_S,svc);assert(!s->lamp_seen);
 svc[19]=0;svc[30]=UINT64_C(0x1402f303c);fr_probe(FR_S,svc);assert(!s->lamp_seen);
 svc[30]=0x402f303c;fr_probe(FR_S,svc);assert(s->lamp_seen==4);
 svc[30]=0x40ca1994;mapping(255);assert(s->lamp_seen==12 && (s->seen&FR_DONE));
 /* M does not depend on S/R; invalid client never causes an OOB map read. */
 reset();prepare();s=before();forbid_io=1;mapping(0);assert(s->lamp_seen==8 && !s->lamp_failed);service(0);assert(s->lamp_seen==8);
 reset();prepare();beg[0]=UINT64_C(0x12345678ffffffff);s=before();forbid_io=1;mapping(0);
 assert(!mapreads && s->lamp_failed==8 && (s->seen&FR_DONE));
 /* Never-run Live baseline is LA_OFF: no prepare needed for lamp evidence. */
 reset();rt->capture.phase=LA_OFF;rt->recorder.arm=1;fr_prepare(rt);assert(!ops);
 s=before();forbid_io=1;complete(s);assert(!ops && s->lamp_seen==15 && s->tx[1].error==0);
 /* Every stock W19 client is accepted; map is read before the first lamp call. */
 for(unsigned client=0;client<5;client++){
  reset();prepare();beg[0]=UINT64_C(0x123400000000)|client;s=before();forbid_io=1;
  service(0);svc[19]=UINT64_C(0x987600000000)|client;maps[client]=42;svc[30]=0x40ca1994;
  expect_map_before_light=1;fr_probe(FR_M,svc);assert(s->lamp_seen==12 && mapreads==1 && !s->lamp_failed);
 }
 /* Thread-safe boot claim: exactly one entry transaction, identity immutable. */
 for(int i=0;i<200;i++){
  reset();prepare();pthread_t a,b;struct fr_state *sa=0,*sb=0;
  assert(!pthread_create(&a,0,entry_thread,&sa));assert(!pthread_create(&b,0,entry_thread,&sb));
  pthread_join(a,0);pthread_join(b,0);assert(!!sa+!!sb==1 && ops==3 && rt->recorder.entry_claimed);
 }
 /* Each failed sleep stops immediately; never spin after DELAY rejection. */
 reset();prepare();s=before();forbid_io=1;can_sleep=0;oldedges=edges;ir(s);
 assert(!delays && edges==oldedges && s->lamp_failed==3);
 reset();prepare();s=before();forbid_io=1;delayfail=1;ir(s);assert(delays==2 && clock_us==0 && !lit && s->lamp_failed==3);
 reset();prepare();s=before();forbid_io=1;freq=0;ir(s);assert(!delays && s->lamp_failed==3);
 /* Per-edge deadlines: 7 ms scheduler quantum stretches 1.5 s to 1.617 s. */
 reset();prepare();s=before();forbid_io=1;quantum=7000;ir(s);assert(clock_us==3234000 && !s->lamp_failed);
 /* Real emitter waveforms, including escapes, decoded by Python test. */
 reset();rt->recorder.arm=1;s=before();wave=fopen("lamp-c-wave.csv","w");assert(wave);fprintf(wave,"time_us,level\n");
 static const uint32_t phases[]={0,1,1,1,1,2,2,2,3,3,3,3};
 static const uint32_t values[]={0,0,1,2,3,0,5,0xdeadbeef,0,1,255,42};
 for(unsigned i=0;i<sizeof(phases)/sizeof(*phases);i++){clock_us+=300000;assert(emit(phases[i],values[i]));}
 clock_us+=300000;s->lamp_seen=3u<<FR_NR_SHIFT;fr_light(s,FR_R,beg);
 clock_us+=300000;assert(emit(FR_D,(17u<<2)|3u));
 clock_us+=300000;light((void*)(uintptr_t)UINT64_C(0xa0280a1b0),0);assert(!fclose(wave));wave=0;
 reset();prepare();forbid_io=1;s=before();fr_light(s,FR_I,beg);
 oldedges=edges;fr_checkpoint(17);assert(edges>oldedges && !(s->missed&FR_LAMP_BUSY));
 fr_light(s,FR_R,beg);oldedges=edges;fr_checkpoint(18);assert(edges==oldedges);
 reset();prepare();forbid_io=1;s=before();oldedges=edges;fr_checkpoint(17);assert(edges==oldedges); /* no I */
 fr_light(s,FR_I,beg);s->missed|=FR_LAMP_BUSY;oldedges=edges;fr_checkpoint(17);assert(edges==oldedges);
 fr_light(s,FR_R,beg);assert(s->lamp_failed&(1u<<FR_R));assert(edges==oldedges); /* stuck owner */
 reset();prepare();forbid_io=1;s=before();fr_light(s,FR_I,beg);
 uint64_t t0=clock_us;cancel_at=t0+100000;fr_checkpoint(17);
 assert(clock_us==t0+100000 && !lit && !(s->missed&FR_LAMP_BUSY));
 assert(!(s->lamp_failed&(1u<<FR_D))); /* expected R cancellation, not error */
 cancel_at=0;fr_light(s,FR_R,beg);assert(!(s->lamp_failed&(1u<<FR_R)));
 reset();prepare();forbid_io=1;s=before();fr_light(s,FR_I,beg);can_sleep=0;oldedges=edges;
 fr_checkpoint(15);assert(edges==oldedges && (s->lamp_failed&(1u<<FR_D)) && !(s->missed&FR_LAMP_BUSY));
 free(rt);puts("PASS: prepare transaction only; identity and failed-sleep tests; nominal 6.000 s / 7 ms quantum 6.468 s; C wave saved");return 0;
}
