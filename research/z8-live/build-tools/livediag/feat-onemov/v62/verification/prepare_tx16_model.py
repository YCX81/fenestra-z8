#!/usr/bin/env python3
"""Prepare a private, pinned TX16 model extension; never changes shared QEMU."""
from pathlib import Path
import difflib, hashlib, json

P=Path(__file__).resolve().parent
old=(P/'tx16-baseline.h').read_text()
assert hashlib.sha256(old.encode()).hexdigest()=='f28c1c2bfa085b58775764c3e9bcbea9a58898e90f2965d17d62daff6bb19f0d'
s=old
def replace(a,b):
    global s
    assert s.count(a)==1,(a,s.count(a))
    s=s.replace(a,b)
replace('#define Z8_AUDIO_TX_BUDGET (16u * 1024u * 1024u)', '''#define Z8_AUDIO_TX_BUDGET (16u * 1024u * 1024u)

/* Pinned stock one-shot resources: tone1=4800; tone10/14/18/22=28800,
 * tone11/15/19/23=9600, tone12/16/20/24=19200. No arbitrary-length sink. */
static bool serial5_tx16_sfx_bytes(unsigned bytes)
{
    return bytes==4800 || bytes==9600 || bytes==19200 || bytes==28800;
}''')
replace('s->serial5_tx16_sfx_model && request==27 && length==4800',
        's->serial5_tx16_sfx_model && request==27 && serial5_tx16_sfx_bytes(length)')
replace('bool sfx=narrow16 && lanes[0]>=0 && lengths[0]==4800;',
        'bool sfx=s->serial5_tx16_sfx_model && lanes[0]>=0 && serial5_tx16_sfx_bytes(lengths[0]);')
replace('bool sfx=s->serial5_tx16_sfx_model && bytes==4800;',
        'bool sfx=s->serial5_tx16_sfx_model && serial5_tx16_sfx_bytes(bytes);')
replace('sfx?"sfx4800":"ring192000"','sfx?"sfx":"ring192000"')
replace('s->serial5_tx16_sfx_model && bytes==4800 ? UINT64_C(25000000)',
        's->serial5_tx16_sfx_model && serial5_tx16_sfx_bytes(bytes) ? (uint64_t)bytes*1000000000/(48000*4)')
(P/'nikon-z8-audio-tx.h').write_text(s)
(P/'native-sfx-model.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/nikon-z8-audio-tx.h',tofile='b/nikon-z8-audio-tx.h')))

def function(text,signature):
    first=text.index(signature);at=text.index('{',first);i=at+1;depth=1
    while depth:
        depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[first:i]

# Reuse only the old harness infrastructure/cases, replacing all model function
# bodies with the actual candidate source verbatim (including clock scaling).
test=(P/'tx16-old-test.c').read_text()
prefix=test[:test.index('static int serial5_tx_drain_request(')]
prefix=prefix.replace('bool serial5_tx_drain_model,','bool serial5_tx_counter_clock,serial5_tx_drain_model,')
names=['static bool serial5_tx16_sfx_bytes(', 'static int serial5_tx_drain_request(',
       'static bool serial5_tx_drain_same(', 'static void serial5_tx_drain_note_write(',
       'static void serial5_tx_drain_tick(', 'static uint64_t serial5_tx_clock_delay(',
       'static void serial5_tx_drain_schedule(', 'static bool serial5_tx_drain_read(']
body='\n'.join(function(s,n) for n in names)+'\n'
tail=test[test.index('static NikonZ8State s;'):]
extra=r'''
 /* Every new exact resource length, ring option, and counter clock. */
 const unsigned lengths[]={4800,9600,19200,28800};
 for(unsigned k=0;k<4;k++)for(unsigned ring=0;ring<2;ring++)for(unsigned counter=0;counter<2;counter++) {
  unsigned n=lengths[k];setup();s.serial5_tx16_sfx_model=1;s.serial5_tx16_ring_model=ring;s.serial5_tx_counter_clock=counter;
  s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b000000u|(n/4-1);
  assert(serial5_tx_drain_request(&s,&source,&bytes)==12&&bytes==n);
  serial5_tx_drain_schedule(&s);uint64_t due=(uint64_t)n*1000000000/(48000*4);if(counter)due=due*984/1000;
  assert(timer.pending&&timer.due==due&&irqs==0&&capture.len==0);
  now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);
  assert(irqs==1&&capture.len==n&&!memcmp(capture.data,ram,n));
  assert(s.serial5_tx16_sfx_jobs==1&&s.serial5_tx16_ring_jobs==0&&s.serial5_tx_drain_phase==0&&!timer.pending);
  uint64_t fifo=9;assert(serial5_tx_drain_read(&s,0x0e20011c,&fifo,4)&&fifo==0);cases++;
 }
 const unsigned bad_lengths[]={4,4796,4804,9596,9604,19196,19204,28796,28804,7680,7712,7744,17024,65536,262144};
 for(unsigned k=0;k<sizeof(bad_lengths)/sizeof(bad_lengths[0]);k++) {
  setup();s.serial5_tx16_sfx_model=1;s.serial5_tx16_ring_model=1;s.peripheral24[13][0]=0;
  s.peripheral24[12][0]=0x9b000000u|(bad_lengths[k]/4-1);
  assert(serial5_tx_drain_request(&s,&source,&bytes)==-1);serial5_tx_drain_schedule(&s);serial5_tx_drain_tick(&s);
  assert(!timer.pending&&!irqs&&!capture.len);cases++;
 }
 /* Unsupported bank, a concurrent REC bank, aliases, flags and format remain rejected for tone10. */
 for(unsigned k=0;k<15;k++) {
  setup();s.serial5_tx16_sfx_model=1;s.serial5_tx16_ring_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b001c1f;
  switch(k) {
   case 0:s.peripheral24[12][0]=0x9c001c1f;break;
   case 1:arm(13,28,1);break;
   case 2:arm(13,27,1);break;
   case 3:s.peripheral24[12][0]|=0x10000;break;
   case 4:s.peripheral24[12][1]|=0x10000;break;
   case 5:s.peripheral24[12][1]^=0x200000;break;
   case 6:s.peripheral24[12][2]|=0x10;break;
   case 7:s.peripheral24[12][3]=1;break;
   case 8:s.peripheral24[12][4]++;break;
   case 9:s.peripheral24[12][4]=0x1000;break;
   case 10:s.peripheral_global[1]=0;break;
   case 11:s.serial5_config[0][0]=0x2000;break;
   case 12:s.serial5_config[0][0]^=0x100;break;
   case 13:s.serial5_config[0][0]|=2;break;
   case 14:s.serial5_audio_output_id="host";break;
  }
  assert(serial5_tx_drain_request(&s,&source,&bytes)==-1);serial5_tx_drain_schedule(&s);serial5_tx_drain_tick(&s);
  assert(!timer.pending&&!irqs&&!capture.len);cases++;
 }
 /* Replacing a short tone with the real tone10 must restart the complete 150ms deadline. */
 setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 serial5_tx_drain_schedule(&s);now=20000000;s.peripheral24[12][0]=0x9b001c1f;
 serial5_tx_drain_schedule(&s);assert(timer.pending&&timer.due==170000000&&!irqs);
 now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);assert(irqs==1&&capture.len==28800);cases++;
 /* No completion for failed source reads or an unscheduled changed DMA owner. */
 for(unsigned k=0;k<2;k++) {
  setup();s.serial5_tx16_sfx_model=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b001c1f;
  serial5_tx_drain_schedule(&s);now=timer.due;timer.pending=0;
  if(k==0)readfail=1;else s.peripheral24[12][4]+=4;
  serial5_tx_drain_tick(&s);assert(s.serial5_tx_drain_fault&&!irqs&&!capture.len);cases++;
 }
'''
marker=' printf("{\\"passed\\":true'
assert tail.count(marker)==1
tail=tail.replace(marker,extra+'\n'+marker)
(P/'test_tx16.c').write_text(prefix+body+tail)
manifest={'baseline_header_sha256':hashlib.sha256(old.encode()).hexdigest(),
          'candidate_header_sha256':hashlib.sha256(s.encode()).hexdigest(),
          'allowed_sfx_bytes':[4800,9600,19200,28800],
          'duration_formula':'bytes / (48000 * 4), then existing counter-clock scale 984/1000 when enabled',
          'functions':{n:hashlib.sha256(function(s,n).encode()).hexdigest() for n in names},
          'limits':'Private functional model. No physical DAC/timing or hardware sound proof; no firmware change.'}
(P/'model-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
