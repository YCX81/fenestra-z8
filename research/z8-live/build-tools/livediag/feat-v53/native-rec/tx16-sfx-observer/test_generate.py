from pathlib import Path
import sys
P=Path(__file__).resolve().parent
s=(P/'baseline-test.c').read_text();h=(P/(sys.argv[1] if len(sys.argv)>1 else 'nikon-z8-audio-tx.h')).read_text().replace('NikonZ9State','NikonZ8State')
def fn(text,name):
 a=text.index('static ',text.index(name)-40) if False else text.rfind('static ',0,text.index(name))
 p=text.index('{',text.index(name));n=1;i=p+1
 while n:
  n+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[a:i]
for name in ['serial5_tx_drain_request(', 'serial5_tx_drain_tick(', 'serial5_tx_drain_read(']:
 s=s.replace(fn(s,name),fn(h,name))
# Skip the forward declaration when extracting schedule.
a=s.index('static void serial5_tx_drain_schedule(NikonZ8State *s)');b=h.index('static void serial5_tx_drain_schedule(NikonZ8State *s)')
s=s[:a]+s[a:].replace(fn(s[a:],'serial5_tx_drain_schedule('),fn(h[b:],'serial5_tx_drain_schedule('),1)
s=s.replace('serial5_tx16_ring_model;','serial5_tx16_ring_model,serial5_tx16_sfx_model;')
s=s.replace('uint64_t serial5_tx_drain_bytes,','uint64_t serial5_tx16_sfx_jobs,serial5_tx16_ring_jobs,serial5_tx_drain_bytes,')
s=s.replace('const char*serial5_tx_drain_fault;', 'int serial5_tx_scheduled_ch;uint64_t serial5_tx_scheduled_source;unsigned serial5_tx_scheduled_bytes;uint32_t serial5_tx_scheduled_global,serial5_tx_scheduled_dma[7],serial5_tx_scheduled_config[8];const char*serial5_tx_drain_fault;')
helpers=fn(h,'serial5_tx_drain_same(')+'\n'+fn(h,'serial5_tx_drain_note_write(')+'\n'
s=s.replace('static void serial5_tx_drain_tick(void *opaque)',helpers+'static void serial5_tx_drain_tick(void *opaque)')
s=s.replace(' printf("{', '''
 /* Exact4800B SFX is separate from192k REC, with no first-block prefill. */
 for(unsigned both=0;both<2;both++) {
  setup();s.serial5_tx16_sfx_model=1;s.serial5_tx16_ring_model=both;
  s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
  assert(serial5_tx_drain_request(&s,&source,&bytes)==12&&bytes==4800);
  serial5_tx_drain_schedule(&s);assert(timer.due==25000000);now=timer.due;timer.pending=0;
  serial5_tx_drain_tick(&s);assert(irqs==1&&capture.len==4800&&!memcmp(capture.data,ram,4800));
  assert(s.serial5_tx16_sfx_jobs==1&&s.serial5_tx16_ring_jobs==0&&s.serial5_tx_drain_phase==0&&!timer.pending);cases++;
 }
 for(unsigned bad=0;bad<14;bad++) {
  setup();s.serial5_tx16_sfx_model=1;s.serial5_tx16_ring_model=1;
  s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
  switch(bad) {
   case0:s.serial5_tx16_sfx_model=0;break;
   case1:s.peripheral24[12][0]=0x9c0004af;break;
   case2:arm(13,28,1);break;
   case3:arm(13,27,1);break;
   case4:s.peripheral24[12][0]=0x9b00ffff;break;
   case5:s.peripheral24[12][0]=0x9b000000;break;
   case6:s.peripheral24[12][0]=0x9b0004ae;break;
   case7:s.serial5_config[0][0]|=2;break;
   case8:s.peripheral24[12][4]++;break;
   case9:s.peripheral24[12][4]=0x1000;break;
   case10:s.serial5_config[0][0]&=~0x2000;break;
   case11:s.serial5_audio_output_id="host";break;
   case12:s.serial5_config[0][0]=0x2000;break;
   case13:s.serial5_config[0][0]^=0x100;break;
  }
  assert(serial5_tx_drain_request(&s,&source,&bytes)==-1);serial5_tx_drain_schedule(&s);serial5_tx_drain_tick(&s);assert(!irqs&&!timer.pending);cases++;
 }
 for(unsigned kind=0;kind<4;kind++) {
  setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
  serial5_tx_drain_schedule(&s);assert(timer.due==25000000);now=20000000;
  if(kind==0)s.peripheral24[12][4]+=4800;
  if(kind==1)s.serial5_config[0][1]^=1;
  if(kind==2)serial5_tx_drain_note_write(&s,12,0);
  if(kind==3){s.peripheral24[12][0]&=~0x80000000u;serial5_tx_drain_schedule(&s);assert(!timer.pending);s.peripheral24[12][0]|=0x80000000u;}
  serial5_tx_drain_schedule(&s);assert(timer.due==45000000&&irqs==0);
  now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);assert(irqs==1&&s.serial5_tx16_sfx_jobs==1);cases++;
 }
 printf("{'''.replace('case0:','case 0:').replace('case1:','case 1:').replace('case2:','case 2:').replace('case3:','case 3:').replace('case4:','case 4:').replace('case5:','case 5:').replace('case6:','case 6:').replace('case7:','case 7:').replace('case8:','case 8:').replace('case9:','case 9:').replace('case10:','case 10:').replace('case11:','case 11:').replace('case12:','case 12:').replace('case13:','case 13:'))
(P/('test_merged.c' if len(sys.argv)>1 else 'test_tx.c')).write_text(s)
