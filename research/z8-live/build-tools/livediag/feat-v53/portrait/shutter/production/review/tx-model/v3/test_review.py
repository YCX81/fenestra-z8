from pathlib import Path
import subprocess,json,hashlib,difflib
P=Path(__file__).resolve().parent;V=P.parents[5]/'native-rec/tx16-counter-observer';H=(V/'nikon-private-tx16.h').read_text();s=(V/'test_counter.c').read_text()
def fn(text,name):
 start=text.rfind('static ',0,text.index(name));j=text.index('{',text.index(name));depth=1;i=j+1
 while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[start:i]
h=H.replace('NikonZ9State','NikonZ8State')
for name in ['serial5_tx_drain_request(', 'serial5_tx_drain_same(', 'serial5_tx_drain_note_write(', 'serial5_tx_drain_tick(', 'serial5_tx_drain_read(', 'serial5_tx_clock_delay(']:s=s.replace(fn(s,name),fn(h,name))
a=s.index('static void serial5_tx_drain_schedule(NikonZ8State *s)');b=h.index('static void serial5_tx_drain_schedule(NikonZ8State *s)');s=s[:a]+s[a:].replace(fn(s[a:],'serial5_tx_drain_schedule('),fn(h[b:],'serial5_tx_drain_schedule('),1)
# Use the exact published production frame bodies, not the reviewed harness copy.
A=P.parents[5]/'audio/duration-work/pb-fix37/runtime.c';F=P.parents[5]/'integration-f/work/pb-fix37/runtime.c'
for label,path in [('F',F),('G',A)]:
 body=fn(path.read_text(),'frame(').replace('static void frame(',f'static void frame_{label}(').replace('lp_jpeg_decode(','decode_stub(')
 s=s.replace(fn(s,f'frame_{label}('),body)
setter=fn(h,'set_serial5_tx_counter_clock(')
extra='''\ntypedef NikonZ8State Object;typedef int Error;static int rejected;\n#define NIKON_Z9_LAB(p) ((NikonZ8State*)(p))\nstatic void error_setg(Error**e,const char*t){(void)e;(void)t;rejected++;}\n'''+setter+'\n'
s=s.replace('int main(void)',extra+'int main(void)')
s=s.replace(' printf("{',r'''
 /* Independent setter, rearm and integer-coordinate checks. */
 setup();assert(!s.serial5_tx_counter_clock);s.serial5_config[0][0]=0;rejected=0;set_serial5_tx_counter_clock(&s,1,0);assert(s.serial5_tx_counter_clock&&!rejected);set_serial5_tx_counter_clock(&s,0,0);assert(!s.serial5_tx_counter_clock&&!rejected);cases++;
 for(unsigned cause=0;cause<4;cause++){setup();s.serial5_config[0][0]=0;rejected=0;if(cause==0)s.serial5_config[0][0]=0x1000;if(cause==1)s.serial5_config[0][0]=0x2000;if(cause==2)s.serial5_tx_drain_jobs=1;if(cause==3)s.serial5_tx_drain_phase=1;set_serial5_tx_counter_clock(&s,1,0);assert(rejected==1&&!s.serial5_tx_counter_clock);cases++;}
 setup();s.serial5_tx_counter_clock=1;assert(serial5_tx_clock_delay(&s,1000000)==984000);assert(serial5_tx_clock_delay(&s,25000000)==24600000);assert(serial5_tx_clock_delay(&s,1000000000)==984000000);s.serial5_tx_counter_clock=0;assert(serial5_tx_clock_delay(&s,1000000000)==1000000000);cases++;
 setup();s.serial5_tx16_sfx_model=1;s.serial5_tx_counter_clock=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;serial5_tx_drain_schedule(&s);now=20000000;serial5_tx_drain_note_write(&s,12,0);serial5_tx_drain_schedule(&s);assert(timer.due==44600000);s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);assert(!timer.pending);serial5_tx_drain_tick(&s);assert(!irqs);cases++;
 setup();s.serial5_tx16_ring_model=1;s.serial5_tx_counter_clock=1;serial5_tx_drain_schedule(&s);now=900000;arm(13,28,2);serial5_tx_drain_schedule(&s);assert(timer.due==984000);cases++;
 setup();s.serial5_tx16_sfx_model=1;s.serial5_tx_counter_clock=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;serial5_tx_drain_schedule(&s);readfail=1;now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);assert(!irqs&&!capture.len&&!strcmp(s.serial5_tx_drain_fault,"mapped-source-read-failed"));cases++;
 printf("{''',1)
(P/'test_review.c').write_text(s);subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined','-I'+str(A.parents[1]/'src-fix37/codec'),str(P/'test_review.c'),'-o',str(P/'test_review')],check=True)
r=subprocess.run([str(P/'test_review')],capture_output=True,text=True);(P/'stdout.txt').write_text(r.stdout);(P/'stderr.txt').write_text(r.stderr);assert r.returncode==0,r.stderr;out=json.loads(r.stdout);assert out['cases']==70
out.update(header_sha256=hashlib.sha256(H.encode()).hexdigest(),lab_sha256=hashlib.sha256((V/'nikon-z9-lab.c').read_bytes()).hexdigest(),pb_fixed_sha256=hashlib.sha256(A.read_bytes()).hexdigest(),checks=['55 old flag-off cases retained','four-cell exact F/newPB with old/newclock endpoint comparison','clock-domain setter rejects active TX/RX, prior TX jobs and pending phase','identical rearm fresh deadline and stop no lateIRQ','future bank rearm preserves current deadline','actual armed RAM read failure remains noIRQ'],boundary='Private functional counter-domain input, exact source bodies with RAM/IRQ/time leaves stubbed; not physical ADC/DAC proof.')
(P/'independent-review.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
