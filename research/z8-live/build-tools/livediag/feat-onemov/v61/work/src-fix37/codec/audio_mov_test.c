/* Host check: v4 MOV writer + appended sowt PCM + live_audio_mov_trak (track 3). */
#include "live_codec.h"
#include "../audio/live_audio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int write_at(void*o,uint64_t at,const void*p,size_t n){FILE*f=o;return fseek(f,at,SEEK_SET)||fwrite(p,1,n,f)!=n;}
struct trak {struct live_audio_info info;uint64_t at;};
static size_t trak(void*v,uint8_t*p,size_t cap){struct trak*t=v;return live_audio_mov_trak(p,cap,&t->info,3,1000000,t->at);}
int main(int argc,char**argv){
 const char*id="4C495645-5048-4F54-8F41-303030303031";
 size_t p,s;assert(argc==2 && !live_encoder_sizes(640,424,&p,&s));
 live_encoder e;void*pm=aligned_alloc(16,(p+15)&~15ul),*sm=aligned_alloc(16,(s+15)&~15ul);uint8_t*frame=aligned_alloc(16,640*424*3/2);
 assert(pm&&sm&&frame&&!live_encoder_init(&e,pm,sm,640,424));
 FILE*f=fopen(argv[1],"w+b");assert(f);live_mov*m=calloc(1,sizeof(*m));assert(!live_mov_begin(m,write_at,f,640,424));
 uint64_t pts=1000000;
 for(unsigned i=0;i<90;i++){
  for(unsigned y=0;y<424;y++)for(unsigned x=0;x<640;x++)frame[y*640+x]=16+((x+y+i*3)%220);
  memset(frame+640*424,128,640*424/2);
  const uint8_t*out;size_t bytes;assert(!live_encoder_frame(&e,frame,&out,&bytes));assert(!live_mov_add(m,out,bytes,pts));pts+=33333;
 }
 /* 3 s window starting at the first video frame; first sample within one sample period. */
 struct trak t={{48000,2,16,143998,1000010,1000000,1000000+90*33333u},0};
 int16_t*pcm=malloc(t.info.frames*4);assert(pcm);
 for(uint32_t i=0;i<t.info.frames;i++){pcm[2*i]=(int16_t)(10000*sin(i*2*M_PI*440/48000));pcm[2*i+1]=(int16_t)(10000*sin(i*2*M_PI*660/48000));}
 assert(!live_mov_append_raw(m,pcm,t.info.frames*4,&t.at));
 assert(!live_mov_finish_with(m,2500000,33333,id,trak,&t));assert(!fclose(f));
 printf("pcm at %llu, %u frames\n",(unsigned long long)t.at,t.info.frames);
 free(pm);free(sm);free(frame);free(m);free(pcm);return 0;
}
