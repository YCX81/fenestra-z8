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
parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2]/'writer/work')
parser.add_argument('--report',type=Path,default=Path(__file__).parent/'pb-audio-review.json')
args=parser.parse_args()
ROOT=args.source
text=(ROOT/'pb-fix37/runtime.c').read_text()
start=text.index('static void audio_begin(')
end=text.index('\n/* One step per worker loop:',start)
body=text[start:end]
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
#include "preview_reader.h"
#define PB_AUDIO_RING_BYTES 480000u
#define PB_AUDIO_PLAY_BYTES 460800u
#define PB_AUDIO_SERVICE 0u
#define PB_AUDIO_DRIVER 1u
#define LOAD(p) (*(p))
static _Alignas(16) uint8_t ring_store[PB_AUDIO_RING_BYTES+16];
#define PB_AUDIO_RING ((uintptr_t)ring_store)
typedef struct {
 LpPreview preview;LpClip clip;
 uint32_t audio_stage,shutdown,audio_bytes,audio_stop;
 uint8_t jpeg[LP_JPEG_MAX+16],audio_payload[32];
} PbRuntime;
static uint8_t *file;static size_t file_size;
static unsigned reads,posts,fail_read;
static int read_at(int fd,uint64_t off,void*p,size_t n){
 assert(fd==7);reads++;if(fail_read&&reads==fail_read)return 0;
 assert(off<=file_size&&n<=file_size-off);memcpy(p,file+off,n);return 1;
}
static void audio_post(PbRuntime*r,uint32_t id){(void)r;assert(id==0x43180014u);posts++;}
__BODY__
static void le32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static int16_t value(uint32_t i){return (int16_t)(i*73u+17u);}
static void trial(unsigned packed,uint32_t frames,unsigned fail,unsigned truncated){
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
  assert(posts==1&&r->audio_stage==1);uint32_t play=frames>115200u?115200u:frames;
  assert(r->audio_bytes==play*4u);
  for(uint32_t i=0;i<play*2u;i++)assert((uint16_t)(ring_store[2u*i]|(uint16_t)ring_store[2u*i+1]<<8)==(uint16_t)value(i));
  for(uint32_t i=play*4u;i<PB_AUDIO_RING_BYTES;i++)assert(!ring_store[i]);
 }
 for(unsigned i=0;i<16;i++){assert(ring_store[PB_AUDIO_RING_BYTES+i]==0xa5);assert(r->jpeg[LP_JPEG_MAX+i]==0x5a);}
 free(file);free(r);
}
int main(void){
 uint32_t counts[]={0,1,32768,32769,115200,192000};
 for(unsigned p=0;p<2;p++){
  for(unsigned i=0;i<sizeof counts/sizeof *counts;i++)trial(p,counts[i],0,0);
  trial(p,192000,3,0);trial(p,192000,0,1);
 }
 puts("PASS LVJA/LVJB exact stereo PCM, read chunk boundaries, 2.4s cap, zero padding, guard bytes, read failure, truncated bounds");
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
