#include "live_audio.h"
/* v50: frozen s16 source is losslessly packed to signed LE24 for stock playback. */
struct atoms {uint8_t *p;size_t n,cap;int bad;};
static __attribute__((noinline)) void byte(struct atoms *a,uint8_t v) {
 if(a->n<a->cap)a->p[a->n]=v;else a->bad=1;
 a->n++;
}
static __attribute__((noinline)) void u16(struct atoms *a,uint16_t v){byte(a,v>>8);byte(a,v);}
static __attribute__((noinline)) void u32(struct atoms *a,uint32_t v){u16(a,v>>16);u16(a,v);}
static __attribute__((noinline)) void u64(struct atoms *a,uint64_t v){u32(a,(uint32_t)(v>>32));u32(a,(uint32_t)v);}
static __attribute__((noinline)) void zero(struct atoms *a,unsigned n){while(n--)byte(a,0);}
static __attribute__((noinline)) void tag(struct atoms *a,const char *s){for(unsigned i=0;i<4;i++)byte(a,(uint8_t)s[i]);}
static __attribute__((noinline)) size_t box(struct atoms *a,const char *s){size_t at=a->n;u32(a,0);tag(a,s);return at;}
static __attribute__((noinline)) void end(struct atoms *a,size_t at){
 if(!a->bad){uint32_t n=(uint32_t)(a->n-at);for(unsigned i=0;i<4;i++)a->p[at+i]=(uint8_t)(n>>(24-8*i));}
}
static __attribute__((noinline)) void matrix(struct atoms *a){
 for(unsigned i=0;i<9;i++)u32(a,i==0||i==4?65536:i==8?0x40000000:0);
}
size_t live_audio_mov_trak(uint8_t *out,size_t cap,const struct live_audio_info *i,
                          uint32_t id,uint32_t scale,uint64_t offset) {
 if(!out || !i || !id || !scale || scale>1000000 || i->rate!=48000 ||
    i->channels!=2 || i->bits!=16 || !i->frames || i->frames>LIVE_AUDIO_CAPACITY ||
    i->window_end_us<=i->window_begin_us || i->first_sample_us<i->window_begin_us ||
    i->first_sample_us>=i->window_end_us ||
    i->first_sample_us-i->window_begin_us>21 || offset>UINT32_MAX-(uint64_t)i->frames*6)return 0;
 uint64_t delay_us=i->first_sample_us-i->window_begin_us;
 uint64_t delay=(delay_us*scale+999999)/1000000;
 uint64_t duration=((uint64_t)i->frames*scale+47999)/48000;
 if(duration+delay>UINT32_MAX)return 0;
 struct atoms a={out,0,cap,0};
 size_t tr=box(&a,"trak"),tk=box(&a,"tkhd");
 u32(&a,7);zero(&a,8);u32(&a,id);u32(&a,0);u32(&a,(uint32_t)(duration+delay));
 zero(&a,8);u16(&a,0);u16(&a,0);u16(&a,0x100);u16(&a,0);matrix(&a);zero(&a,8);end(&a,tk);
 if(delay) {
  size_t ed=box(&a,"edts"),el=box(&a,"elst");
  u32(&a,0x01000000);u32(&a,2);
  u64(&a,delay);u64(&a,UINT64_MAX);u16(&a,1);u16(&a,0);
  u64(&a,duration);u64(&a,0);u16(&a,1);u16(&a,0);end(&a,el);end(&a,ed);
 }
 size_t md=box(&a,"mdia"),mh=box(&a,"mdhd");
 u32(&a,0);zero(&a,8);u32(&a,48000);u32(&a,i->frames);u16(&a,0);u16(&a,0);end(&a,mh);
 size_t hd=box(&a,"hdlr");u32(&a,0);tag(&a,"mhlr");tag(&a,"soun");zero(&a,12);
 byte(&a,10);const char name[]="Live audio";for(unsigned j=0;j<10;j++)byte(&a,(uint8_t)name[j]);end(&a,hd);
 size_t mi=box(&a,"minf"),sm=box(&a,"smhd");u32(&a,0);zero(&a,4);end(&a,sm);
 size_t di=box(&a,"dinf"),dr=box(&a,"dref");u32(&a,0);u32(&a,1);
 size_t url=box(&a,"url ");u32(&a,1);end(&a,url);end(&a,dr);end(&a,di);
 size_t st=box(&a,"stbl"),sd=box(&a,"stsd");u32(&a,0);u32(&a,1);
 static const uint8_t desc[72]={
  0,0,0,72,'l','p','c','m',0,0,0,0,0,0,0,1,0,2,0,0,0,0,0,0,
  0,3,0,16,255,254,0,0,0,1,0,0,0,0,0,72,64,231,112,0,0,0,0,0,
  0,0,0,2,127,0,0,0,0,0,0,24,0,0,0,12,0,0,0,6,0,0,0,1};
 for(unsigned j=0;j<sizeof desc;j++)byte(&a,desc[j]);end(&a,sd);
 size_t ts=box(&a,"stts");u32(&a,0);u32(&a,1);u32(&a,i->frames);u32(&a,1);end(&a,ts);
 uint32_t chunks=(i->frames+24023u)/24024u,last=i->frames-(chunks-1u)*24024u;
 size_t sc=box(&a,"stsc");u32(&a,0);u32(&a,chunks>1&&last!=24024u?2:1);
 u32(&a,1);u32(&a,chunks==1?last:24024u);u32(&a,1);
 if(chunks>1&&last!=24024u){u32(&a,chunks);u32(&a,last);u32(&a,1);}end(&a,sc);
 size_t sz=box(&a,"stsz");u32(&a,0);u32(&a,6);u32(&a,i->frames);end(&a,sz);
 size_t co=box(&a,"stco");u32(&a,0);u32(&a,chunks);
 for(uint32_t j=0;j<chunks;j++)u32(&a,(uint32_t)(offset+(uint64_t)j*24024u*6u));end(&a,co);
 end(&a,st);end(&a,mi);end(&a,md);end(&a,tr);return a.bad?0:a.n;
}
