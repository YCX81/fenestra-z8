from pathlib import Path
import hashlib,json,subprocess
P=Path(__file__).resolve().parent;SRC=P.parents[5]/'native-rec/tx16-sfx-observer/test_tx.c'
s=SRC.read_text();h=(P/'nikon-private-tx16.h').read_text().replace('NikonZ9State','NikonZ8State')
def fn(text,name):
 start=text.rfind('static ',0,text.index(name));j=text.index('{',text.index(name));depth=1;i=j+1
 while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[start:i]
for name in ['serial5_tx_drain_request(', 'serial5_tx_drain_same(', 'serial5_tx_drain_note_write(', 'serial5_tx_drain_tick(', 'serial5_tx_drain_read(']:s=s.replace(fn(s,name),fn(h,name))
a=s.index('static void serial5_tx_drain_schedule(NikonZ8State *s)');b=h.index('static void serial5_tx_drain_schedule(NikonZ8State *s)');s=s[:a]+s[a:].replace(fn(s[a:],'serial5_tx_drain_schedule('),fn(h[b:],'serial5_tx_drain_schedule('),1)
s=s.replace(' printf("{',r'''
 /* Independent checks of actual selected tuple, memory errors and off switch. */
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 s.serial5_config[0][0]=0x2000;assert(serial5_tx_drain_request(&s,&source,&bytes)==-1);serial5_tx_drain_schedule(&s);serial5_tx_drain_tick(&s);assert(!irqs&&!timer.pending);cases++;
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 serial5_tx_drain_schedule(&s);now=20000000;s.peripheral24[12][4]+=192000;serial5_tx_drain_schedule(&s);assert(timer.due==45000000);now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);assert(irqs==1&&!memcmp(capture.data,ram+192000,4800));cases++;
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 serial5_tx_drain_schedule(&s);s.peripheral24[12][4]+=192000;timer.pending=0;serial5_tx_drain_tick(&s);assert(!irqs&&!capture.len&&!strcmp(s.serial5_tx_drain_fault,"pending-tuple-changed"));cases++;
 setup();s.serial5_tx16_ring_model=1;serial5_tx_drain_schedule(&s);assert(timer.due==1000000);now=900000;arm(13,28,2);serial5_tx_drain_schedule(&s);assert(timer.due==1000000);cases++;
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;serial5_tx_drain_schedule(&s);readfail=1;timer.pending=0;serial5_tx_drain_tick(&s);assert(!irqs&&!capture.len&&!strcmp(s.serial5_tx_drain_fault,"mapped-source-read-failed"));cases++;
 setup();s.serial5_tx16_ring_model=1;serial5_tx_drain_schedule(&s);s.serial5_tx_drain_bytes=Z8_AUDIO_TX_BUDGET;timer.pending=0;serial5_tx_drain_tick(&s);assert(!irqs&&!strcmp(s.serial5_tx_drain_fault,"bounded-tx-budget-exhausted"));cases++;
 setup();s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;serial5_tx_drain_schedule(&s);serial5_tx_drain_tick(&s);assert(!irqs&&!timer.pending);cases++;
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;serial5_tx_drain_schedule(&s);now=20000000;s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);assert(!timer.pending);s.serial5_config[0][0]|=0x2000;serial5_tx_drain_schedule(&s);assert(timer.due==45000000);s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);serial5_tx_drain_tick(&s);assert(!irqs);cases++;
 printf("{''',1)
(P/'test_review.c').write_text(s)
subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(P/'test_review.c'),'-o',str(P/'test_review')],check=True)
r=subprocess.run([str(P/'test_review')],capture_output=True,text=True);(P/'test_review.stdout').write_text(r.stdout);(P/'test_review.stderr').write_text(r.stderr);assert r.returncode==0,r.stderr
res=json.loads(r.stdout);assert res['cases']==63
out=dict(passed=True,cases=63,header_sha256=hashlib.sha256((P/'nikon-private-tx16.h').read_bytes()).hexdigest(),lab_sha256=hashlib.sha256((P/'nikon-z9-lab.c').read_bytes()).hexdigest(),sanitizers='ASan+UBSan',v1_counterexamples_fixed=True,tests=['v1 unknown mode rejects withoutIRQ','v1 source rewrite receives fresh deadline and exact newbytes','unexpected tuple change withoutschedule failsclosed','future bank rearm doesnot reset active bank deadline','realRAMreaderror afterproperarm gives noIRQ','budget error afterproperarm gives noIRQ','both optins off neveracceptsTX even masteron','real disable/rearm fresh25ms and late stoppedcallback noIRQ'],boundary='Actual model core functions, RAM/address translation and PDMA leaf stubs. Word0 mode450a2000 is observed; functional48k/25ms model timing is declared, not a physical clock or DAC proof.')
(P/'independent-review.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
