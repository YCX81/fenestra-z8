#include "work/src-fix37/codec/live_transaction.h"
#include "work/src-fix37/codec/preview_writer.h"
#include "work/src-fix37/codec/preview_reader.h"
#include "work/src-fix37/audio/live_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
static const char UUID[37]="12345678-1234-1234-1234-123456789ABC";
typedef struct {FILE*f[4];char root[1024];const uint8_t*frame,*pcm;uint32_t af;int fault;unsigned removed,frames;} test;
static uint32_t le32(const uint8_t*p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t*p){return le32(p)|(uint64_t)le32(p+4)<<32;}
static uint8_t *load(const char*p,size_t*n){FILE*f=fopen(p,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long z=ftell(f);assert(z>=0);rewind(f);uint8_t*b=malloc(z);assert(b);assert(fread(b,1,z,f)==(size_t)z);fclose(f);*n=z;return b;}
static int openr(void*v,const char*p,int*out){(void)v;(void)p;(void)out;return -1;}
static const char*suffix(const char*p){return strstr(p,".MOV")?"MOV":strstr(p,".MAN")?"MAN":"JPG";}
static int create(void*v,const char*p,int*out){test*t=v;const char*s=suffix(p);int fd=*s=='M'&&s[1]=='O'?1:*s=='M'?2:3;char name[1200];snprintf(name,sizeof name,"%s.%s",t->root,s);t->f[fd]=fopen(name,"w+bx");if(!t->f[fd])return -1;*out=fd;return 0;}
static int seek_(void*v,int fd,uint64_t at,uint64_t*got){test*t=v;if(fseek(t->f[fd],(long)at,SEEK_SET))return -1;*got=ftell(t->f[fd]);return 0;}
static int read_(void*v,int fd,void*p,size_t n,size_t*got){test*t=v;*got=fread(p,1,n,t->f[fd]);return ferror(t->f[fd])?-1:0;}
static int write_(void*v,int fd,const void*p,size_t n,size_t*got){test*t=v;if(t->fault==2&&n==8&&!memcmp((const uint8_t*)p+4,"NCTH",4))return -1;*got=fwrite(p,1,n,t->f[fd]);return ferror(t->f[fd])?-1:0;}
static int close_(void*v,int fd){test*t=v;int rc=fclose(t->f[fd]);t->f[fd]=0;return rc;}
static int remove_(void*v,const char*p){test*t=v;char name[1200];snprintf(name,sizeof name,"%s.%s",t->root,suffix(p));t->removed++;return remove(name);}
static int cancel_(void*v){test*t=v;return t->fault==3&&t->frames>0;}
static int ensure(void*v,const char*p){(void)v;(void)p;return 0;}
static int preview(void*v,uint32_t i,void*p,size_t n){test*t=v;assert(i<45&&n==LIVE_PREVIEW_FRAME_BYTES);if(t->fault==1)return -1;memcpy(p,t->frame,n);t->frames++;return 0;}
static int audio(void*v,uint32_t off,int16_t*p,uint32_t cap,uint32_t*got){test*t=v;if(off>t->af)return -1;uint32_t n=t->af-off;if(n>cap)n=cap;memcpy(p,t->pcm+off*4,n*4);*got=n;return 0;}
static size_t trak(void*v,uint8_t*p,size_t cap,uint64_t at){test*t=v;struct live_audio_info i={48000,2,16,t->af,1000000,1000000,1000000+(uint64_t)t->af*1000000/48000};return live_audio_mov_trak(p,cap,&i,3,1000000,at);}
int live_encoder_codec(void){return LIVE_CODEC_HEVC;}
int live_encoder_init(live_encoder*a,void*b,void*c,unsigned d,unsigned e){(void)a;(void)b;(void)c;(void)d;(void)e;return -1;}
int live_encoder_frame(live_encoder*a,const uint8_t*b,const uint8_t**c,size_t*d){(void)a;(void)b;(void)c;(void)d;return -1;}
int main(int argc,char**argv){assert(argc==6);test t={0};snprintf(t.root,sizeof t.root,"%s",argv[4]);t.fault=atoi(argv[5]);size_t an,pn,jn;
 uint8_t*ab=load(argv[1],&an),*pcm=load(argv[2],&pn),*jpeg=load(argv[3],&jn);assert(!(pn%4));t.pcm=pcm;t.af=pn/4;
 uint8_t*frame=malloc(LIVE_PREVIEW_FRAME_BYTES),*scratch=malloc(LIVE_PREVIEW_FRAME_BYTES);LpJpeg*dec=calloc(1,sizeof *dec);assert(frame&&scratch&&dec);assert(!lp_jpeg_decode(dec,jpeg,jn,frame,1024,682));t.frame=frame;
 live_input_au au[LIVE_MAX_FRAMES];unsigned count=0;for(size_t at=0;at<an;){assert(at+12<=an&&count<LIVE_MAX_FRAMES);uint32_t n=le32(ab+at);uint64_t pts=le64(ab+at+4);at+=12;assert(n<=an-at);au[count++]=(live_input_au){ab+at,n,pts};at+=n;}
 live_still_ticket ticket={.native_path="A:/DCIM/100NCZ_8/DSC_0028.JPG",.native_size=1,.press_us=2500000,.volume_generation=1,.closed_and_registered=1,.has_jpeg=0,
  .preview_frame=preview,.preview_ctx=&t,.preview_scratch=scratch,.audio_get=audio,.audio_trak=trak,.audio_ctx=&t,.audio_frames=t.af};memcpy(ticket.uuid,UUID,36);
 live_file_ops ops={.opaque=&t,.open_read=openr,.create_exclusive=create,.seek=seek_,.read=read_,.write=write_,.close=close_,.remove_owned=remove_,.cancelled=cancel_,.ensure_dir=ensure};
 live_transaction_workspace*w=calloc(1,sizeof *w);unsigned cleanup;int rc=live_write_pair_aus(w,&ops,&ticket,au,count,1024,682,33367,&cleanup);
 if(t.fault){assert(rc&&cleanup==0&&t.removed==1&&!t.f[1]&&!t.f[2]);printf("PASS rollback fault=%d\n",t.fault);}else {assert(!rc&&!cleanup&&t.frames==46);printf("PASS full writer %u HEVC, %u audio, %u previews\n",count,t.af,t.frames);}
 free(w);free(dec);free(frame);free(scratch);free(ab);free(pcm);free(jpeg);return 0;
}
