#include "live_codec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int write_at(void*o,uint64_t at,const void*p,size_t n){FILE*f=o;return fseek(f,at,SEEK_SET)||fwrite(p,1,n,f)!=n;}
int main(int argc,char**argv){
 const char*id="4C495645-5048-4F54-8F41-303030303031";
 size_t p,s;assert(!live_encoder_sizes(640,424,&p,&s));printf("persist=%zu scratch=%zu ring=%u total_no_stack=%zu\n",p,s,640*424*3/2*90,p+s+640*424*3/2*90+sizeof(live_mov));
 live_encoder e;void*pm=aligned_alloc(16,(p+15)&~15ul),*sm=aligned_alloc(16,(s+15)&~15ul);uint8_t*frame=aligned_alloc(16,640*424*3/2);assert(pm&&sm&&frame);assert(!live_encoder_init(&e,pm,sm,640,424));
 char path[1024];assert(argc==3);snprintf(path,sizeof(path),"%s/test.mov",argv[2]);FILE*f=fopen(path,"w+b");assert(f);live_mov*m=calloc(1,sizeof(*m));assert(!live_mov_begin(m,write_at,f,640,424));
 uint64_t pts=1000000;for(unsigned i=0;i<90;i++){
 for(unsigned y=0;y<424;y++)for(unsigned x=0;x<640;x++)frame[y*640+x]=16+((x+y+i*3)%220);
 memset(frame+640*424,100+i%40,640*424/4);memset(frame+640*424*5/4,160-i%40,640*424/4);
 const uint8_t*out;size_t bytes;assert(!live_encoder_frame(&e,frame,&out,&bytes));assert(!live_mov_add(m,out,bytes,pts));pts+=i==40?66666:33333;
 }
 assert(!live_mov_finish(m,2500000,33333,id));assert(!fclose(f));
 if(argc>1){f=fopen(argv[1],"rb");assert(f);uint8_t*head=malloc(65540);size_t got=fread(head,1,65540,f);assert(!ferror(f));assert(!live_jpeg_pair_prefix(head,got,id));snprintf(path,sizeof(path),"%s/paired.JPG",argv[2]);FILE*out=fopen(path,"wb");assert(out);assert(fwrite(head,1,got,out)==got);while((got=fread(head,1,65540,f)))assert(fwrite(head,1,got,out)==got);assert(!ferror(f));assert(!fclose(f));assert(!fclose(out));free(head);}
 free(pm);free(sm);free(frame);free(m);return 0;
}
