/* Offline fixture using the exact production MOV and Nikon metadata writers.
 * Encoded AUs are copied, one real center image supplies four Nikon thumbnails.
 * Deliberately no45-frame LVJ1 preview and no audio in this dimension-only case. */
#include "live_codec.h"
#include "preview_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static live_mov movie;
static unsigned char au[1024*1024],center[LIVE_PREVIEW_FRAME_BYTES],scratch[LIVE_PREVIEW_FRAME_BYTES],io[65536];
static int write_at(void*c,uint64_t p,const void*d,size_t n){FILE*f=c;return fseek(f,p,SEEK_SET)||fwrite(d,1,n,f)!=n;}
static int frame(void*c,uint32_t i,void*d,size_t n){(void)c;assert(i==LIVE_PREVIEW_FRAMES/2&&n==sizeof center);memcpy(d,center,n);return 0;}
int main(int argc,char**argv){
 assert(argc==3);FILE*f=fopen(argv[1],"wbx"),*raw=fopen(argv[2],"rb");assert(f&&raw);assert(fread(center,1,sizeof center,raw)==sizeof center);assert(fgetc(raw)==EOF);assert(!fclose(raw));
 assert(!live_mov_begin_codec(&movie,write_at,f,1280,852,LIVE_CODEC_HEVC));
 for(unsigned i=0;i<90;i++){
  unsigned char b[4];assert(fread(b,1,4,stdin)==4);unsigned n=(unsigned)b[0]<<24|(unsigned)b[1]<<16|(unsigned)b[2]<<8|b[3];assert(n&&n<=sizeof au&&fread(au,1,n,stdin)==n);
  assert(!live_mov_add(&movie,au,n,(uint64_t)i*1001000/30));
 }
 assert(fgetc(stdin)==EOF);assert(!live_mov_finish_prefix(&movie,1501500,33367,"85200000-0265-4000-8000-000000000001",0,0));
 LivePreviewWriter w={f,frame,write_at,0};LiveNikonFacts facts={1280,852,90,30000,1001,3,0};uint64_t end=0;
 assert(!live_nikon_metadata_write(&w,scratch,sizeof scratch,io,sizeof io,movie.pos,&end,&facts));assert(!live_mov_extend_moov(&movie,end));assert(!fclose(f));printf("%u bytes,90frames,noLVJ1\n",movie.pos);return 0;
}
