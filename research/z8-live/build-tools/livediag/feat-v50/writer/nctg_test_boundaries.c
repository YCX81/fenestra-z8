#include "nctg-work/src-fix37/codec/preview_writer.h"
#include "nctg-work/src-fix37/codec/preview_reader.h"
#include "nctg-work/src-fix37/audio/live_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int baseline_preview_jpeg(const LivePreviewWriter*,const uint8_t*,unsigned,unsigned,unsigned,uint8_t*,size_t,uint64_t,uint32_t,uint32_t*);
typedef struct {uint8_t *data;size_t cap;unsigned calls;} dst;
static int wr(void*v,uint64_t at,const void*p,size_t n){dst*d=v;assert(at<=d->cap&&n<=d->cap-at);memcpy(d->data+at,p,n);d->calls++;return 0;}
static int noisy_last(void*v,uint32_t i,void*p,size_t n){
 (void)v;assert(n==LIVE_PREVIEW_FRAME_BYTES);
 if(i<44)memset(p,128,n);else {unsigned state=12345;for(size_t k=0;k<n;k++){state=state*1664525u+1013904223u;((uint8_t*)p)[k]=(uint8_t)(state>>24);}}
 return 0;
}
static void le(uint8_t*p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=x>>(8*i);}
int main(void){
 uint8_t rec[16]={'L','V','J','A'};le(rec+4,40);le(rec+8,10);le(rec+12,48000);LpClip c={0};c.atom=100;
 uint32_t at=0,n=0;assert(!lp_clip_audio(rec,&c,&at,&n)&&at==40&&n==10);rec[3]='B';assert(!lp_clip_audio(rec,&c,&at,&n));c.atom=99;assert(lp_clip_audio(rec,&c,&at,&n));rec[3]='A';assert(!lp_clip_audio(rec,&c,&at,&n));rec[3]='C';assert(lp_clip_audio(rec,&c,&at,&n));
 uint8_t atom[2048];memset(atom,0xa5,sizeof atom);struct live_audio_info info={48000,2,16,192000,1000000,1000000,5000000};
 assert(live_audio_mov_trak(atom,sizeof atom-16,&info,3,1000000,100));for(unsigned i=sizeof atom-16;i<sizeof atom;i++)assert(atom[i]==0xa5);
 assert(!live_audio_mov_trak(atom,64,&info,3,1000000,100));assert(!live_audio_mov_trak(atom,sizeof atom,&info,3,1000000,UINT32_MAX-100));
 const unsigned w=1024,h=682;uint8_t*frame=malloc((size_t)w*h*3/2),*b1=malloc(4<<20),*b2=malloc(4<<20),*io=malloc(4096);assert(frame&&b1&&b2&&io);
 unsigned state=12345;for(unsigned i=0;i<w*h*3/2;i++){state=state*1664525u+1013904223u;frame[i]=(uint8_t)(state>>24);}
 dst d1={b1,4<<20,0},d2={b2,4<<20,0};LivePreviewWriter p1={&d1,0,wr,0},p2={&d2,0,wr,0};uint32_t n1,n2;
 assert(!baseline_preview_jpeg(&p1,frame,w,h,45,io,4096,0,4<<20,&n1));
 assert(!live_preview_jpeg(&p2,frame,w,h,45,io,4096,0,4<<20,&n2));assert(n1==n2&&!memcmp(b1,b2,n1));
 assert(live_preview_jpeg_scaled(&p2,frame,w,h,1280,720,35,io,4096,0,100,&n2)==1);
 uint64_t end=0;assert(live_nikon_metadata_write(&p2,frame,1024,io,4096,0,&end,&(LiveNikonFacts){w,h,95,30000,1001,3,1}));
 p2.frame=noisy_last;uint64_t pe=0;assert(!live_preview_write(&p2,frame,LIVE_PREVIEW_FRAME_BYTES,io,4096,1024,&pe,40,10));
 LpClip clip;LpPreview pr;uint64_t atom_at;uint32_t atom_bytes;
 assert(!lp_clip_tail(b2+pe-8,pe,&atom_at,&atom_bytes)&&atom_at==1024);
 assert(!lp_clip_open(b2+atom_at,8+LIVE_PREVIEW_HEADER,atom_at,atom_bytes,&pr,&clip,1));
 assert(!lp_clip_audio(b2+clip.at[pr.frames],&clip,&at,&n)&&at==40&&n==10);
 assert(clip.end>clip.at[pr.frames]+16); /* oversized retry residue exists */
 printf("PASS oversized final JPEG retains adjacent audio record\n");
 printf("PASS LVJA/LVJB bounds, audio capacity/32-bit offsets, preview byte identity (%u B noisy frame), JPEG limit, NCDT scratch bound\n",n1);
 free(frame);free(b1);free(b2);free(io);return 0;
}
