#!/usr/bin/env python3
"""Exercise the actual PB audio_begin body with host-only device/I/O stubs."""
from pathlib import Path
import re
import subprocess
import tempfile
import hashlib
import json
import argparse

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parent/'work')
parser.add_argument('--report',type=Path,default=Path(__file__).parent/'pb-audio-review.json')
args=parser.parse_args()
ROOT=args.source
text=(ROOT/'pb-fix37/runtime.c').read_text()
start=text.index('static void audio_begin(')
end=text.index('\n/* v37: the preview is inside',start)
body=text[text.index('int pb_audio_handoff(void)'):text.index('static void audio_callback(')]+text[start:end]
# Host has no device memory map or ARM cache maintenance instructions. Keep the
# format validation, read sizes, PCM conversion, padding and state code intact.
body=body.replace('base<UINT64_C(0x800000000)||base>UINT64_C(0xa40000000)-PB_AUDIO_RING_BYTES',
                  'base!=(uintptr_t)ring_store')
body=body.replace('for(uint32_t i=0;i<PB_AUDIO_RING_BYTES;i+=64u)__asm__ volatile("dc cvac, %0"::"r"(ring+i):"memory");','')
body=body.replace('__asm__ volatile("dsb sy":::"memory");','')
template=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "preview_reader.h"
#define PB_AUDIO_RING_BYTES 960000u
#define PB_AUDIO_PLAY_BYTES 768000u
static uint32_t service,driver=1,capacity=1000000;
#define PB_AUDIO_CAPACITY capacity
#define PB_AUDIO_SERVICE service
#define PB_AUDIO_DRIVER driver
#define LOAD(p) (*(p))
#define STORE(p,v) (*(p)=(v))
static uint64_t now=1000000;
static uint64_t now_us(void){return now;}
static _Alignas(64) uint8_t ring_store[PB_AUDIO_RING_BYTES+16];
#define PB_AUDIO_RING ((uintptr_t)ring_store)
typedef struct {
 LpPreview preview;LpClip clip;
 uint32_t audio_stage,shutdown,audio_bytes,audio_stop;
 uint32_t audio_ack,audio_errors,audio_plays,playing;
 uint32_t audio_waits,audio_skipped,audio_reason,audio_source_frames;
 uint32_t audio_pending;
 _Atomic uint32_t audio_gate;uint32_t audio_stop_retries;
 int preview_fd;
 uint64_t audio_t;
 uint8_t jpeg[LP_JPEG_MAX+16],audio_payload[32];
} PbRuntime;
static uint8_t *file;static size_t file_size;
static PbRuntime *current_runtime;
static PbRuntime *pb_runtime_get(void){return current_runtime;}
int pb_audio_handoff(void);
static unsigned reads,posts,fail_read,hold_in_read;
static int read_at(int fd,uint64_t off,void*p,size_t n){
 assert(fd==7);reads++;if(fail_read&&reads==fail_read)return 0;
 assert(off<=file_size&&n<=file_size-off);memcpy(p,file+off,n);
 if(hold_in_read&&reads>1){assert(!pb_audio_handoff());hold_in_read=0;}
 return 1;
}
static uint32_t last_post;
static void audio_callback(uint32_t id,void*ctx){(void)id;(void)ctx;}
static void audio_post(PbRuntime*r,uint32_t id){assert(id==0x43180014u||id==0x4318001au||id==0x43180015u||id==0x43180016u);posts++;last_post=id;r->audio_ack=0;r->audio_t=now;}
__BODY__
static void le32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static int16_t value(uint32_t i){return (int16_t)(i*73u+17u);}
static void trial(unsigned packed,uint32_t frames,unsigned fail,unsigned truncated){
 service=0;driver=1;
 uint32_t frame_bytes=packed?6u:4u;file_size=32u+(size_t)frames*frame_bytes;
 file=calloc(1,file_size);assert(file);memcpy(file,packed?"LVJB":"LVJA",4);
 le32(file+4,32);le32(file+8,frames);le32(file+12,48000);
 for(uint32_t i=0;i<frames*2u;i++){
  uint16_t n=(uint16_t)value(i);uint8_t*p=file+32u+(size_t)i*(packed?3u:2u);
  if(packed){p[0]=0;p[1]=(uint8_t)n;p[2]=(uint8_t)(n>>8);}
  else {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
 }
 PbRuntime*r=calloc(1,sizeof *r);assert(r);
 r->preview.frames=1;r->clip.at[1]=0;r->clip.atom=file_size-truncated;
 memset(ring_store,0xa5,sizeof ring_store);memset(r->jpeg+LP_JPEG_MAX,0x5a,16);
 reads=posts=0;fail_read=fail;
 audio_begin(r,7);
 if(!frames||fail||truncated){assert(!posts&&!r->audio_stage);}
 else {
  assert(posts==1&&r->audio_stage==1);uint32_t play=frames;
  assert(r->audio_bytes==play*4u);
  for(uint32_t i=0;i<play*2u;i++)assert((uint16_t)(ring_store[2u*i]|(uint16_t)ring_store[2u*i+1]<<8)==(uint16_t)value(i));
  for(uint32_t i=play*4u;i<PB_AUDIO_RING_BYTES;i++)assert(!ring_store[i]);
 }
 for(unsigned i=0;i<16;i++){assert(ring_store[PB_AUDIO_RING_BYTES+i]==0xa5);assert(r->jpeg[LP_JPEG_MAX+i]==0x5a);}
 free(file);free(r);
}
static PbRuntime *wait_setup(void){
 PbRuntime*r=calloc(1,sizeof *r);assert(r);file_size=64;file=calloc(1,file_size);assert(file);
 memcpy(file,"LVJB",4);le32(file+4,32);le32(file+8,4);le32(file+12,48000);
 r->preview.frames=1;r->clip.at[1]=0;r->clip.atom=file_size;
 r->preview_fd=7;r->playing=1;reads=posts=fail_read=hold_in_read=0;service=0;driver=5;current_runtime=r;return r;
}
static void wait_tests(void){
 PbRuntime*r=wait_setup();audio_begin(r,7);assert(r->audio_stage==7&&!posts&&r->audio_waits==1);
 uint64_t wait_t=r->audio_t;now+=1000000;audio_step(r);assert(r->audio_stage==7&&r->audio_t==wait_t&&!posts);
 driver=1;audio_step(r);assert(r->audio_stage==1&&posts==1&&r->audio_reason==0);
 r->audio_ack=last_post;r->audio_payload[0x14]=1;audio_step(r);assert(r->audio_stage==2&&last_post==0x4318001a);
 r->audio_ack=last_post;r->audio_payload[0x15]=1;audio_step(r);assert(r->audio_stage==3&&last_post==0x43180015);
 r->audio_ack=last_post;r->audio_payload[0x14]=1;audio_step(r);assert(r->audio_stage==4&&r->audio_plays==1);
 now+=100000;audio_step(r);assert(r->audio_stage==5&&last_post==0x43180016);
 r->audio_ack=last_post;r->audio_payload[0]=1;audio_step(r);assert(!r->audio_stage);free(r);free(file);
 r=wait_setup();service=1;driver=1;audio_begin(r,7);assert(r->audio_stage==7&&r->audio_reason==2);
 now+=1500000;audio_step(r);assert(!r->audio_stage&&!posts&&r->audio_skipped==1&&r->audio_reason==6);free(r);free(file);
 r=wait_setup();audio_begin(r,7);r->audio_stop=1;r->preview_fd=-1;audio_step(r);assert(!r->audio_stage&&!posts);free(r);free(file);
 r=wait_setup();audio_begin(r,7);r->shutdown=1;audio_step(r);assert(!r->audio_stage&&!posts);free(r);free(file);
 puts("PASS busy microphone/service waits then starts; open/buffer/start/stop replies; bounded timeout, leave and shutdown cancel without native ownership");
}
static void lifecycle_tests(void){
 PbRuntime*r=wait_setup();driver=1;capacity=959999;audio_begin(r,7);
 assert(!posts&&!r->audio_stage&&r->audio_reason==4);capacity=1000000;
 audio_begin(r,7);assert(r->audio_stage==1);r->audio_stop=1;
 r->audio_ack=last_post;r->audio_payload[0x14]=1;audio_step(r);assert(r->audio_stage==5);
 /* Re-enter REC while old stop is pending: don't overwrite the old message. */
 unsigned n=posts;audio_begin(r,7);assert(r->audio_pending&&posts==n);
 audio_step(r);assert(r->audio_stage==5&&posts==n);
 /* Negative stop ACK retries only while the failed old service owns it. */
 r->audio_ack=last_post;service=2;now+=20000;audio_step(r);assert(r->audio_stage==5&&posts==n+1);
 /* Successful ACK followed by a NEW owner must never post another stop. */
 r->audio_ack=last_post;r->audio_payload[0]=1;driver=3;service=2;now+=20000;audio_step(r);
 assert(!r->audio_stage&&posts==n+1&&r->audio_pending&&!r->audio_gate);
 driver=1;service=0;
 audio_step(r);assert(r->audio_stage==1&&!r->audio_pending&&last_post==0x43180014);
 /* Lost ACK retains native resources, clears pending video hold, never reposts. */
 r->audio_pending=1;n=posts;now+=700001;audio_step(r);assert(r->audio_stage==6&&!r->audio_pending&&posts==n);
 audio_begin(r,7);assert(!r->audio_pending&&r->audio_reason==7&&posts==n);
 free(r);free(file);
 r=wait_setup();driver=1;hold_in_read=1;audio_begin(r,7);
 assert(!posts&&!r->audio_stage&&(r->audio_gate&3u)==1u&&pb_audio_handoff());
 assert(!audio_reopen(r,0)); /* stale REC ticket must not undo handoff */
 assert(audio_reopen(r,r->audio_gate));assert(!(r->audio_gate&3u));
 atomic_store(&r->audio_gate,0);audio_begin(r,7);assert(r->audio_stage==1&&!pb_audio_handoff());
 r->audio_ack=last_post;r->audio_payload[0x14]=1;audio_step(r);assert(r->audio_stage==5);
 n=posts;service=2;
 for(unsigned i=0;i<4;i++){r->audio_ack=last_post;now+=20000;audio_step(r);}
 assert(r->audio_stage==6&&posts==n+3&&!pb_audio_handoff());
 free(r);free(file);current_runtime=0;
 puts("PASS actual buffer capacity guard; max3 failed-stop retries; new-owner success ACK no second STOP; queued REC and lost-ACK retain; handoff closes copy/post admission");
}
int main(void){
 uint32_t counts[]={0,1,32768,32769,115200,192000};
 for(unsigned p=0;p<2;p++){
  for(unsigned i=0;i<sizeof counts/sizeof *counts;i++)trial(p,counts[i],0,0);
  trial(p,192000,3,0);trial(p,192000,0,1);
 }
 puts("PASS LVJA/LVJB exact full 0..4s stereo PCM, read chunk boundaries, >=1s zero tail, guard bytes, read failure, truncated bounds");
 wait_tests();lifecycle_tests();
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='pb-audio-review-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(template.replace('__BODY__',body))
    subprocess.run(['clang','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I',str(ROOT/'src-fix37/codec'),str(p/'test.c'),
                    str(ROOT/'src-fix37/codec/preview_reader.c'),'-o',str(p/'test')],check=True)
    r=subprocess.run([str(p/'test')],capture_output=True,text=True)
    print(r.stdout,end='');print(r.stderr,end='')
    args.report.write_text(json.dumps({
        'passed':r.returncode==0,'runtime_sha256':hashlib.sha256(text.encode()).hexdigest(),
        'body_sha256':hashlib.sha256(text[start:end].encode()).hexdigest(),
        'stdout':r.stdout,'stderr':r.stderr,'host_adaptations':['mock device address range','omit ARM cache maintenance']},indent=2)+'\n')
    raise SystemExit(r.returncode)
