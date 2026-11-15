from pathlib import Path
import hashlib,json,subprocess
P=Path(__file__).resolve().parent;SRC=P.parents[4]/'native-rec/tx16-sfx-observer/test_tx.c'
s=SRC.read_text();h=(P/'nikon-private-tx16.h').read_text().replace('NikonZ9State','NikonZ8State')
def fn(text,name):
 start=text.rfind('static ',0,text.index(name));j=text.index('{',text.index(name));depth=1;i=j+1
 while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[start:i]
for name in ['serial5_tx_drain_request(', 'serial5_tx_drain_tick(', 'serial5_tx_drain_read(']:s=s.replace(fn(s,name),fn(h,name))
a=s.index('static void serial5_tx_drain_schedule(NikonZ8State *s)');b=h.index('static void serial5_tx_drain_schedule(NikonZ8State *s)');s=s[:a]+s[a:].replace(fn(s[a:],'serial5_tx_drain_schedule('),fn(h[b:],'serial5_tx_drain_schedule('),1)
s=s.replace(' printf("{',r'''
 /* Independent boundary probes; preserve original successful/bad-tuple suite. */
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 s.serial5_config[0][0]=0x2000;
 assert(serial5_tx_drain_request(&s,&source,&bytes)==12); /* format fields ignored */
 serial5_tx_drain_schedule(&s);now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);
 assert(irqs==1&&s.serial5_tx16_sfx_jobs==1);fprintf(stderr,"REVIEW unknown-config accepted with25ms IRQ\n");
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 serial5_tx_drain_schedule(&s);assert(timer.due==25000000);now=20000000;
 s.peripheral24[12][4]+=192000;serial5_tx_drain_schedule(&s);
 assert(timer.due==25000000);now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);
 assert(irqs==1&&!memcmp(capture.data,ram+192000,4800));fprintf(stderr,"REVIEW valid descriptor rewrite inherits5ms residual deadline\n");
 /* Legitimate stop/rearm does cancel and require a fresh25ms. */
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 serial5_tx_drain_schedule(&s);now=20000000;s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);assert(!timer.pending);
 s.peripheral24[12][4]+=192000;s.serial5_config[0][0]|=0x2000;serial5_tx_drain_schedule(&s);assert(timer.due==45000000);
 /* Direct stop before queued callback never publishes completion. */
 s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);serial5_tx_drain_tick(&s);assert(irqs==0);
 printf("{''',1)
(P/'test_review.c').write_text(s)
cc=['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(P/'test_review.c'),'-o',str(P/'test_review')]
subprocess.run(cc,check=True);r=subprocess.run([str(P/'test_review')],capture_output=True,text=True);(P/'test_review.stdout').write_text(r.stdout);(P/'test_review.stderr').write_text(r.stderr);assert r.returncode==0,r.stderr
report=dict(suite_passed=True,original_case_count=49,header_sha256=hashlib.sha256((P/'nikon-private-tx16.h').read_bytes()).hexdigest(),counterexamples=['serial_config00002000 lacks supported format fields but receives25ms completion','active valid source rewrite retains prior deadline; new source completed after5ms'],passing_independent_boundaries=['TXdisable cancels pendingtimer','re-enable schedules fresh25ms','stopped callback cannot emitIRQ'],scope='Exact merged request/tick/schedule/read source compiled withASan/UBSan; hardware RAM/PDMA leaf mocks. Counterexamples concern model admission/ownership, not asserted actual whole-run behavior.')
(P/'independent-review.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
