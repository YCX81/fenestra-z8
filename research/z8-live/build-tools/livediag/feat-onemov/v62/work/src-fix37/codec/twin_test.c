/* Host test for live_write_twin (second-card Live copy). Usage: twin_test NATIVE.JPG
 * Path-keyed fake card: the first card's set is A:, the twin goes to B:. Checks the
 * copied MOV, the rebuilt pairing JPG, the twin manifest, every I/O failure point,
 * cancellation, collisions, path guards and that the first card is never written. */
#define _GNU_SOURCE
#include "live_transaction.h"
#include "live_codec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAXF 16
typedef struct {char path[128];uint8_t *data;size_t size;int exists,open,written;} file;
typedef struct {file f[MAXF];size_t pos[MAXF];unsigned calls,fail,cancel_after,yields,remove_fail,dirs;char dir[8][128];} card;
static int fault(card*c){return ++c->calls==c->fail;}
static file*find(card*c,const char*p){for(unsigned i=0;i<MAXF;i++)if(c->f[i].exists&&!strcmp(c->f[i].path,p))return &c->f[i];return 0;}
static int openr(void*v,const char*p,int*out){card*c=v;if(fault(c))return -1;file*f=find(c,p);if(!f)return -1;int i=(int)(f-c->f);f->open=1;c->pos[i]=0;*out=i;return 0;}
static int create(void*v,const char*p,int*out){card*c=v;if(fault(c)||find(c,p))return -1;
 for(unsigned i=0;i<MAXF;i++)if(!c->f[i].exists){file*f=&c->f[i];snprintf(f->path,128,"%s",p);f->data=realloc(f->data,48u<<20);f->size=0;f->exists=f->open=f->written=1;c->pos[i]=0;*out=(int)i;return 0;}
 return -1;}
static int seek_(void*v,int fd,uint64_t at,uint64_t*actual){card*c=v;if(fault(c))return -1;c->pos[fd]=at;*actual=at;return 0;}
static int read_(void*v,int fd,void*p,size_t n,size_t*got){card*c=v;if(fault(c))return -1;file*f=&c->f[fd];assert(f->open);
 if(c->pos[fd]>f->size||n>f->size-c->pos[fd])return -1;memcpy(p,f->data+c->pos[fd],n);c->pos[fd]+=n;*got=n;return 0;}
static int write_(void*v,int fd,const void*p,size_t n,size_t*got){card*c=v;file*f=&c->f[fd];assert(f->open&&f->written);if(fault(c))return -1;
 memcpy(f->data+c->pos[fd],p,n);c->pos[fd]+=n;if(f->size<c->pos[fd])f->size=c->pos[fd];*got=n;return 0;}
static int close_(void*v,int fd){card*c=v;assert(c->f[fd].open);c->f[fd].open=0;return fault(c)?-1:0;}
static int remove_(void*v,const char*p){card*c=v;file*f=find(c,p);assert(f&&f->written);if(c->remove_fail)return -1;f->exists=0;return 0;}
static int cancelled(void*v){card*c=v;return c->cancel_after&&c->yields>=c->cancel_after;}
static void yield_(void*v){((card*)v)->yields++;}
static int ensure(void*v,const char*p){card*c=v;if(fault(c))return -1;assert(p[0]=='B');if(c->dirs<8)snprintf(c->dir[c->dirs++],128,"%s",p);return 0;}
static void put(card*c,unsigned i,const char*p,const uint8_t*d,size_t n){file*f=&c->f[i];snprintf(f->path,128,"%s",p);f->data=realloc(f->data,48u<<20);memcpy(f->data,d,n);f->size=n;f->exists=1;f->open=f->written=0;}
static const char*A_NATIVE="A:\\DCIM\\100NCZ_8\\DSC_0001.JPG",*B_NATIVE="B:\\DCIM\\100NCZ_8\\DSC_0001.JPG";
static const char UUID[]="4C495645-5048-4F54-8F41-303030303031";
static uint8_t *jpeg,*mov,*bjpeg;static size_t jn,mn;
static void setup(card*c){
 for(unsigned i=0;i<MAXF;i++){c->f[i].exists=0;c->f[i].open=0;}
 c->calls=c->fail=c->cancel_after=c->yields=c->remove_fail=c->dirs=0;
 put(c,0,A_NATIVE,jpeg,jn);put(c,1,"A:\\LIVE\\100NCZ_8\\DSC_0001.MOV",mov,mn);put(c,2,B_NATIVE,bjpeg,jn);
}
static unsigned b_files(card*c){unsigned n=0;for(unsigned i=0;i<MAXF;i++)if(c->f[i].exists&&c->f[i].path[0]=='B'&&c->f[i].written)n++;return n;}
static int twin(live_transaction_workspace*w,live_file_ops*ops,unsigned*cleanup){return live_write_twin(w,ops,A_NATIVE,mn,B_NATIVE,jn,UUID,5,cleanup);}
int main(int argc,char**argv){
 assert(argc==2);FILE*src=fopen(argv[1],"rb");assert(src);jpeg=malloc(40u<<20);jn=fread(jpeg,1,40u<<20,src);fclose(src);assert(jn>200000);
 bjpeg=malloc(jn);memcpy(bjpeg,jpeg,jn);bjpeg[jn-3]^=0x5a;           /* the B still differs (JPEG+JPEG roles) */
 mn=3u*65540u+12345u;mov=malloc(mn);for(size_t i=0;i<mn;i++)mov[i]=(uint8_t)(i*131u+7u);
 uint8_t*expect=malloc(jn);memcpy(expect,bjpeg,jn);assert(!live_jpeg_pair_prefix(expect,jn<65540?jn:65540,UUID));
 static card c;live_file_ops ops={&c,openr,create,seek_,read_,write_,close_,remove_,cancelled,yield_,0,ensure};
 live_transaction_workspace*w=calloc(1,sizeof(*w));unsigned cleanup=9;
 setup(&c);assert(!twin(w,&ops,&cleanup)&&!cleanup);unsigned calls=c.calls;
 file*tj=find(&c,"B:\\LIVE\\100NCZ_8\\DSC_0001.JPG"),*tm=find(&c,"B:\\LIVE\\100NCZ_8\\DSC_0001.MOV"),*tf=find(&c,"B:\\LIVEDATA\\100NCZ_8\\DSC_0001.MAN");
 assert(tj&&tm&&tf&&b_files(&c)==3);
 assert(tm->size==mn&&!memcmp(tm->data,mov,mn));
 assert(tj->size==jn&&!memcmp(tj->data,expect,jn));
 char want[256];int k=snprintf(want,256,"native=%s\nsize=%zu\nuuid=%s\nvolume_generation=5\n",B_NATIVE,jn,UUID);
 assert(tf->size==(size_t)k&&!memcmp(tf->data,want,(size_t)k));
 assert(!memcmp(c.f[0].data,jpeg,jn)&&!memcmp(c.f[1].data,mov,mn)&&!c.f[0].written&&!c.f[1].written);
 int sawlive=0,sawdata=0;for(unsigned i=0;i<c.dirs;i++){if(!strcmp(c.dir[i],"B:\\LIVE\\100NCZ_8"))sawlive=1;if(!strcmp(c.dir[i],"B:\\LIVEDATA\\100NCZ_8"))sawdata=1;}
 assert(sawlive&&sawdata);
 /* every single failure point leaves nothing on B and closes every handle */
 for(unsigned i=1;i<=calls;i++){setup(&c);c.fail=i;assert(twin(w,&ops,&cleanup));assert(!b_files(&c)&&!cleanup);
  for(unsigned f=0;f<MAXF;f++)assert(!c.f[f].open);}
 setup(&c);c.cancel_after=3;assert(twin(w,&ops,&cleanup)==-2&&!b_files(&c));
 setup(&c);c.cancel_after=3;c.remove_fail=1;assert(twin(w,&ops,&cleanup)==-2&&cleanup);
 /* an existing file on B is never overwritten or removed */
 setup(&c);put(&c,3,"B:\\LIVE\\100NCZ_8\\DSC_0001.MOV",(const uint8_t*)"keep",4);c.f[3].written=0;
 assert(twin(w,&ops,&cleanup)&&find(&c,"B:\\LIVE\\100NCZ_8\\DSC_0001.MOV")&&!memcmp(c.f[3].data,"keep",4)&&!b_files(&c));
 /* path guards: same volume, other stem/folder, missing sizes */
 setup(&c);assert(live_write_twin(w,&ops,A_NATIVE,mn,A_NATIVE,jn,UUID,5,&cleanup)==-1&&!c.calls);
 assert(live_write_twin(w,&ops,A_NATIVE,mn,"B:\\DCIM\\100NCZ_8\\DSC_0002.JPG",jn,UUID,5,&cleanup)==-1&&!c.calls);
 assert(live_write_twin(w,&ops,A_NATIVE,mn,"B:\\DCIM\\101NCZ_8\\DSC_0001.JPG",jn,UUID,5,&cleanup)==-1&&!c.calls);
 assert(live_write_twin(w,&ops,A_NATIVE,0,B_NATIVE,jn,UUID,5,&cleanup)==-1&&!c.calls);
 assert(live_write_twin(w,&ops,A_NATIVE,mn,B_NATIVE,0,UUID,5,&cleanup)==-1&&!c.calls);
 printf("PASS twin copy: MOV %zu B byte-identical, pairing JPG rebuilt from the twin, manifest names B:, %u failure points clean, cancel, collision, guards\n",mn,calls);
 free(w);free(jpeg);free(bjpeg);free(mov);free(expect);for(unsigned i=0;i<MAXF;i++)free(c.f[i].data);return 0;
}
