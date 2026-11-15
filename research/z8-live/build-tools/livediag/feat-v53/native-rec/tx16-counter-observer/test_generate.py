from pathlib import Path
P=Path(__file__).resolve().parent;F=P.parents[1]
h=(P/'nikon-private-tx16.h').read_text().replace('NikonZ9State','NikonZ8State')
s=(P.parent/'tx16-sfx-observer/test_merged.c').read_text()
def fn(text,name):
 a=text.rfind('static ',0,text.index(name));p=text.index('{',text.index(name));n=1;i=p+1
 while n:n+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[a:i]
a=s.index('static void serial5_tx_drain_schedule(NikonZ8State *s)')
s=s[:a]+s[a:].replace(fn(s[a:],'serial5_tx_drain_schedule('),fn(h,'serial5_tx_clock_delay(')+'\n'+fn(h[h.index('static void serial5_tx_drain_schedule('):],'serial5_tx_drain_schedule('),1)
s=s.replace('serial5_tx16_sfx_model;','serial5_tx16_sfx_model,serial5_tx_counter_clock;',1)
pb=r'''
#define LOAD(p) (*(p))
#define PB_SCENE_DATA_BYTES 4096u
#define FX_OK 0
#include "preview_reader.h"
typedef struct {
 LpPreview preview;LpClip clip;uint64_t start_us,audio_t;uint32_t key_epoch;
 uint32_t audio_stage,audio_stop,audio_bytes,playing,blocked_key_epoch,last_frame,clip_mode,decode_errors,current_id,frames;
 int preview_fd;uint8_t jpeg[32],i420[32];void *decoder,*pixels;int scene,commands[4];
} PbRuntime;
static void hide(PbRuntime*r){r->playing=0;if(r->audio_stage)r->audio_stop=1;}
static int read_at(int f,uint64_t a,void*p,uint32_t n){return 1;}
static int decode_stub(void*d,const void*j,uint32_t n,void*i,uint32_t w,uint32_t h){return 0;}
static int ready_to_write(PbRuntime*r,uint32_t key){return 1;}
static int pb_scene_frame(int*s,int*c,void*p,uint32_t n,void*i,uint32_t fb){return 0;}
static int current(uint32_t *id){*id=1;return 1;}
static int present(PbRuntime*r,uint32_t key){return 1;}
'''
base=(F/'integration-f/work/pb-fix37/runtime.c').read_text();fixed=(F/'audio/duration-work/pb-fix37/runtime.c').read_text()
for name,src in [('frame_F',base),('frame_G',fixed)]:pb+=fn(src,'frame(PbRuntime').replace('static void frame(','static void '+name+'(').replace('lp_jpeg_decode(','decode_stub(')+'\n'
reader=(F/'integration-f/work/src-fix37/codec/preview_reader.c').read_text();a=reader.index('int lp_preview_index(');b=reader.index('\n}',a)+2
pb=pb.replace('static void hide',reader[a:b]+'\nstatic void hide',1)
pb+=r'''
static unsigned endpoint_case(bool counter,bool fixed) {
 setup();s.serial5_tx16_ring_model=1;s.serial5_tx_counter_clock=counter;
 PbRuntime r={0};r.preview=(LpPreview){1024,682,45,1047552,3000015,9};r.key_epoch=9;
 r.playing=1;r.current_id=1;r.preview_fd=7;r.pixels=(void*)1;r.last_frame=44;r.audio_stage=4;r.audio_bytes=144144u*4;
 serial5_tx_drain_schedule(&s);
 for(uint64_t guest=0;guest<=3003000;guest=guest<3000000?guest+1000:guest==3000000?3000015:guest==3000015?3001000:3003000) {
  uint64_t target=guest*984;
  while(timer.pending&&timer.due<=target){now=timer.due;timer.pending=0;unsigned before=s.serial5_tx_drain_jobs;serial5_tx_drain_tick(&s);assert(s.serial5_tx_drain_jobs==before+1);arm(12+(before&1),27+(before&1),(before+2)%5);serial5_tx_drain_schedule(&s);}
  now=target;if(fixed)frame_G(&r,guest);else frame_F(&r,guest);
  if(!r.playing){s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);break;}
  assert(guest<3003000);
 }
 assert(!timer.pending&&r.audio_stop);return s.serial5_tx_drain_jobs;
}
'''
s=s.replace('int main(void)',pb+'\nint main(void)',1)
extra=r'''
 /* Counter-clock is an opt-in conversion, never a firmware deadline change. */
 setup();s.serial5_tx16_sfx_model=1;s.serial5_tx_counter_clock=1;s.peripheral24[13][0]=0;s.peripheral24[12][0]=0x9b0004af;
 serial5_tx_drain_schedule(&s);assert(timer.due==24600000);now=20000000;s.peripheral24[12][4]+=4800;serial5_tx_drain_schedule(&s);assert(timer.due==44600000);
 now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);assert(irqs==1);cases++;
 setup();s.serial5_tx16_ring_model=1;s.serial5_tx_counter_clock=1;serial5_tx_drain_schedule(&s);assert(timer.due==984000);now=timer.due;timer.pending=0;serial5_tx_drain_tick(&s);assert(timer.due==984984000);s.serial5_config[0][0]&=~0x2000;serial5_tx_drain_schedule(&s);assert(!timer.pending);cases++;
 assert(endpoint_case(false,false)==3);cases++; /* oldmodel+F: deficit */
 assert(endpoint_case(false,true)==3);cases++;  /* oldclock can falsely rejectfixedPB */
 assert(endpoint_case(true,false)==3);cases++;  /* correctedclock must NOT hideFbug */
 assert(endpoint_case(true,true)==4);cases++;   /* actualfixedPB reaches last bank */
'''
s=s.replace(' printf("{',extra+' printf("{',1)
(P/'test_counter.c').write_text(s)
