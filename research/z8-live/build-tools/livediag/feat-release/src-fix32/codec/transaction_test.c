#define _GNU_SOURCE
#include "live_transaction.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint8_t *data[4];size_t size[4],pos[4];int exists[4],opened[4];unsigned calls,fail,shortio,badseek,cancel_after,yields,removed,remove_fail;} fs;
static int fault(fs*f){return ++f->calls==f->fail;}
static int openr(void*v,const char*p,int*out){fs*f=v;(void)p;if(fault(f))return -1;*out=0;f->opened[0]=1;f->pos[0]=0;return 0;}
static int create(void*v,const char*p,int*out){fs*f=v;int i=strstr(p,".JPG")?1:strstr(p,".MOV")?2:3;if(fault(f)||f->exists[i])return -1;*out=i;f->exists[i]=f->opened[i]=1;f->size[i]=f->pos[i]=0;return 0;}
static int seek_(void*v,int fd,uint64_t at,uint64_t*actual){fs*f=v;if(fault(f)||at>2000000)return -1;f->pos[fd]=at;*actual=at+(f->calls==f->badseek);return 0;}
static int read_(void*v,int fd,void*p,size_t n,size_t*got){fs*f=v;if(fault(f)||n>f->size[fd]-f->pos[fd])return -1;*got=n-(f->calls==f->shortio);memcpy(p,f->data[fd]+f->pos[fd],*got);f->pos[fd]+=*got;return 0;}
static int write_(void*v,int fd,const void*p,size_t n,size_t*got){fs*f=v;assert(fd!=0);if(fault(f)||n>2000000-f->pos[fd])return -1;*got=n-(f->calls==f->shortio);memcpy(f->data[fd]+f->pos[fd],p,*got);f->pos[fd]+=*got;if(f->size[fd]<f->pos[fd])f->size[fd]=f->pos[fd];return 0;}
static int close_(void*v,int fd){fs*f=v;assert(f->opened[fd]);f->opened[fd]=0;return fault(f)?-1:0;}
static int remove_(void*v,const char*p){fs*f=v;int i=strstr(p,".JPG")?1:strstr(p,".MOV")?2:3;assert(f->exists[i]);f->removed|=1u<<i;if(f->remove_fail)return -1;f->exists[i]=0;return 0;}
static int cancelled(void*v){fs*f=v;return f->cancel_after&&f->yields>=f->cancel_after;}
static void yield_(void*v){((fs*)v)->yields++;}
static int ensure(void*v,const char*p){(void)p;return fault(v)?-1:0;}
static void reset(fs*f){f->calls=f->fail=f->shortio=f->badseek=f->cancel_after=f->yields=f->removed=f->remove_fail=0;memset(f->opened,0,sizeof(f->opened));f->exists[1]=f->exists[2]=f->exists[3]=0;}
static live_input_au aus[3];static int compressed;
static int pair_(live_transaction_workspace*w,const live_file_ops*f,const live_still_ticket*t,const live_input_frame*frames,unsigned n,uint32_t last,void*p,void*s,unsigned*cleanup){
 return compressed?live_write_pair_aus(w,f,t,aus,n,640,424,last,cleanup):live_write_pair(w,f,t,frames,n,last,p,s,cleanup);
}
int main(int argc,char**argv){assert(argc==2||argc==3);compressed=argc==3;fs f={0};for(unsigned i=0;i<4;i++)f.data[i]=malloc(2000000);FILE*src=fopen(argv[1],"rb");assert(src);f.size[0]=fread(f.data[0],1,131080,src);assert(f.size[0]>65540);assert(!fclose(src));uint8_t*original=malloc(f.size[0]);memcpy(original,f.data[0],f.size[0]);
 live_file_ops ops={&f,openr,create,seek_,read_,write_,close_,remove_,cancelled,yield_,0,ensure};live_still_ticket t={.native_path="A:/DCIM/100NCZ_8/DSC_0001.JPG",.native_size=f.size[0],.press_us=1033333,.volume_generation=3,.closed_and_registered=1,.has_jpeg=1,.uuid="4C495645-5048-4F54-8F41-303030303031"};live_transaction_workspace*w=calloc(1,sizeof(*w));size_t p,s;assert(!live_encoder_sizes(640,424,&p,&s));void*pm=aligned_alloc(16,(p+15)&~15ul),*sm=aligned_alloc(16,(s+15)&~15ul);uint8_t*frame=aligned_alloc(16,640*424*3/2);memset(frame,128,640*424*3/2);live_input_frame frames[3]={{frame,1000000},{frame,1033333},{frame,1066666}};unsigned cleanup;
 if(compressed){live_encoder e;assert(!live_encoder_init(&e,pm,sm,640,424));for(unsigned i=0;i<3;i++){const uint8_t *au;size_t n;assert(!live_encoder_frame(&e,frame,&au,&n));uint8_t *b=malloc(n);memcpy(b,au,n);aus[i]=(live_input_au){b,n,frames[i].pts_us};}}
 assert(!pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup));unsigned calls=f.calls;assert(f.exists[1]&&f.exists[2]&&f.exists[3]&&!cleanup);assert(!memcmp(original,f.data[0],f.size[0]));assert(memmem(f.data[3],f.size[3],"volume_generation=3",19));
 for(unsigned i=1;i<=calls;i++){reset(&f);f.fail=i;assert(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup));assert(!f.exists[1]&&!f.exists[2]&&!f.exists[3]&&!cleanup);assert(!f.opened[0]&&!f.opened[1]&&!f.opened[2]&&!f.opened[3]);}
 unsigned short_detected=0,seek_detected=0;for(unsigned i=1;i<=calls;i++){reset(&f);f.shortio=i;if(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup)){short_detected++;assert(!f.exists[1]&&!f.exists[2]);}reset(&f);f.badseek=i;if(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup)){seek_detected++;assert(!f.exists[1]&&!f.exists[2]);}}
 reset(&f);f.exists[1]=1;f.data[1][0]=0xab;assert(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup));assert(f.exists[1]&&f.data[1][0]==0xab&&!f.removed);
 reset(&f);f.exists[2]=1;f.data[2][0]=0xcd;assert(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup));assert(!f.exists[1]&&f.exists[2]&&f.data[2][0]==0xcd&&f.removed==2);
 reset(&f);f.cancel_after=2;assert(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup)==-2);assert(!f.exists[1]&&!f.exists[2]);
 reset(&f);f.cancel_after=2;f.remove_fail=1;assert(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup)==-2);assert(cleanup==3);
 reset(&f);t.closed_and_registered=0;assert(pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup));assert(!f.calls);t.closed_and_registered=1;
 reset(&f);t.has_jpeg=0;assert(!pair_(w,&ops,&t,frames,3,33333,pm,sm,&cleanup));assert(!f.exists[1]&&f.exists[2]);
 assert(!memcmp(original,f.data[0],f.size[0]));printf("PASS checked I/O failures=%u short transfers=%u wrong seek offsets=%u; cancellation, exclusive collisions, orphan reporting, ticket guard, NEF-only, unchanged source\n",calls,short_detected,seek_detected);
 for(unsigned i=0;i<3;i++)free((void *)aus[i].annexb);for(unsigned i=0;i<4;i++)free(f.data[i]);free(original);free(w);free(pm);free(sm);free(frame);return 0;
}
