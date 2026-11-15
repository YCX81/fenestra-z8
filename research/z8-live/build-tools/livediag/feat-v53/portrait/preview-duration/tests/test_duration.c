
#include "preview_writer.h"
#include "preview_reader.h"
#include "live_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CAP (16u*1024u*1024u)
static unsigned calls,writes;static unsigned char *mem;
static int frame(void *p,uint32_t index,void*out,size_t n){calls++;return live_stream_preview_frame(p,index,out,n);}
static int write_(void*p,uint64_t at,const void*d,size_t n){(void)p;assert(at+n<=CAP);memcpy(mem+at,d,n);writes++;return 0;}
static unsigned le32(const unsigned char*p){return p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;}
int main(int argc,char**argv){assert(argc==2);mem=calloc(1,CAP);struct live_stream*s=calloc(1,sizeof *s);unsigned char*raw=malloc(LIVE_PREVIEW_FRAME_BYTES),*io=malloc(65540);assert(mem&&s&&raw&&io);s->preview=malloc((size_t)LIVE_PREVIEW_FRAME*LIVE_PREVIEW_SLOTS);assert(s->preview);s->preview_count=50;s->preview_head=13;s->clip_start=10000000;
 for(unsigned i=0;i<50;i++){unsigned k=(s->preview_head+i)%50;memset(s->preview+(size_t)k*LIVE_PREVIEW_FRAME,16+i,LIVE_PREVIEW_W*LIVE_PREVIEW_H);memset(s->preview+(size_t)k*LIVE_PREVIEW_FRAME+LIVE_PREVIEW_W*LIVE_PREVIEW_H,128,LIVE_PREVIEW_W*LIVE_PREVIEW_H/2);}
 unsigned spans[]={3069733,2002000,3000015,3503500,3999960,1499985};
 for(unsigned c=0;c<sizeof spans/sizeof spans[0];c++){unsigned d=spans[c];s->clip_end=s->clip_start+d;
  /* Actual preview ring: 50 chronological points, wraparound, oldest may be later than clip start. */
  for(unsigned j=0;j<50;j++)s->preview_pts[(s->preview_head+j)%50]=s->clip_end-50u*66667u+(uint64_t)j*66667u;
  for(unsigned i=0;i<45;i++){assert(!live_stream_preview_frame(s,i,raw,LIVE_PREVIEW_FRAME));uint64_t target=s->clip_start+(uint64_t)d*i/45;unsigned pick=0;for(unsigned j=0;j<50;j++)if(s->preview_pts[(s->preview_head+j)%50]<=target)pick=j;assert(raw[0]==16+pick);}
  LivePreviewWriter w={s,frame,write_,0};uint64_t end=0,base=3000000;calls=writes=0;
#ifdef OLD_WRITER
  int rc=live_preview_write(&w,raw,LIVE_PREVIEW_FRAME_BYTES,io,65540,base,&end,32,c%2?0:147347);
#else
  int rc=live_preview_write(&w,raw,LIVE_PREVIEW_FRAME_BYTES,io,65540,base,&end,32,c%2?0:147347,d);
#endif
  assert(!rc&&calls==45&&writes>45);unsigned dt=le32(mem+base+24);assert((uint64_t)dt*45>=d && (uint64_t)dt*45-d<45);
  uint64_t at;uint32_t size;LpPreview p;LpClip clip;assert(!lp_clip_tail(mem+end-8,end,&at,&size)&&at==base);assert(!lp_clip_open(mem+base,LP_CLIP_HEAD,at,size,&p,&clip,9));assert(p.frames==45&&p.duration_us==dt*45&&p.duration_us<=4000000);unsigned index;assert(!lp_preview_index(&p,d-1,9,&index)&&index==44);assert(!lp_preview_index(&p,p.duration_us-1,9,&index)&&index==44);assert(lp_preview_index(&p,p.duration_us,9,&index)==1);assert(lp_preview_index(&p,0,8,&index)==-1);
  uint32_t audio_at,frames;int ar=lp_clip_audio(mem+clip.at[45],&clip,&audio_at,&frames);assert(c%2?ar<0:ar==0&&audio_at==32&&frames==147347);
  char path[512];snprintf(path,sizeof path,"%s/span-%u.lvj",argv[1],d);FILE*f=fopen(path,"wb");assert(f);assert(fwrite(mem+base,1,end-base,f)==end-base);fclose(f);
  printf("span=%u dt=%u shown=%u audio=%u\n",d,dt,p.duration_us,c%2?0:147347);
 }
#ifndef OLD_WRITER
 unsigned invalid[]={0,1,1000000,3999961,4000000,0xffffffffu};LivePreviewWriter w={s,frame,write_,0};
 for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){uint64_t end=0;calls=writes=0;assert(live_preview_write(&w,raw,LIVE_PREVIEW_FRAME_BYTES,io,65540,0,&end,0,0,invalid[i])<0);assert(!calls&&!writes);}
#endif
 assert(live_stream_preview_frame(s,45,raw,LIVE_PREVIEW_FRAME)<0);s->preview_count=0;assert(live_stream_preview_frame(s,0,raw,LIVE_PREVIEW_FRAME)<0);s->preview_count=50;
 uint64_t badend[]={s->clip_start,s->clip_start-1,s->clip_start+4000001};for(unsigned i=0;i<3;i++){s->clip_end=badend[i];assert(live_stream_preview_frame(s,0,raw,LIVE_PREVIEW_FRAME)<0);}
 free(s->preview);free(s);free(raw);free(io);free(mem);puts("PASS duration headers, actual sampling, bounds and old reader");return 0;}
