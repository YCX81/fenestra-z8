/* SPDX-License-Identifier: GPL-2.0-or-later
 * MOV metadata layout derived from .live-photo-20261030/live_hook.c.
 * No filesystem calls, allocation, hidden clocks, or assumed frame rate. */
#include "live_codec.h"
#include <string.h>
#define TAG(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(d))
static uint8_t *put(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;return p+4;}
static uint8_t *zero(uint8_t*p,size_t n){memset(p,0,n);return p+n;}
static uint8_t *copy(uint8_t*p,const void*s,size_t n){memcpy(p,s,n);return p+n;}
static uint8_t *box(uint8_t*p,uint32_t tag){return put(p+4,tag);}
static uint8_t *full(uint8_t*p,uint32_t tag){return zero(box(p,tag),4);}
static uint8_t *end(uint8_t*s,uint8_t*p){put(s,p-s);return p;}
#define B(p,t) box(p,TAG t)
#define F(p,t) full(p,TAG t)
static int writeat(live_mov*m,uint32_t off,const void*p,size_t n){if(m->failed)return -1;if(m->write(m->opaque,off,p,n)){m->failed=1;return -1;}return 0;}
static int append(live_mov*m,const void*p,size_t n){if(n>0xffffffffu-m->pos){m->failed=1;return -1;}if(writeat(m,m->pos,p,n))return -1;m->pos+=n;return 0;}
int live_mov_begin_codec(live_mov*m,live_write_at w,void*o,unsigned width,unsigned height,live_codec_type codec){
 uint8_t h[32]={0};if(!m||!w||!width||!height||width>65535||height>65535||(codec!=LIVE_CODEC_AVC&&codec!=LIVE_CODEC_HEVC))return -1;memset(m,0,sizeof(*m));m->write=w;m->opaque=o;m->width=width;m->height=height;m->codec=codec;
 put(h,24);put(h+4,TAG('f','t','y','p'));put(h+8,TAG('q','t',' ',' '));put(h+16,TAG('q','t',' ',' '));put(h+20,TAG('n','i','k','o'));put(h+28,TAG('m','d','a','t'));return append(m,h,32);
}
int live_mov_begin(live_mov*m,live_write_at w,void*o,unsigned width,unsigned height){
 return live_mov_begin_codec(m,w,o,width,height,LIVE_CODEC_AVC);
}
typedef struct {const uint8_t *p;unsigned bits,at;int bad;} bits;
static unsigned getbits(bits*b,unsigned n){unsigned v=0;if(n>32||b->at>b->bits||n>b->bits-b->at){b->bad=1;return 0;}while(n--){v=(v<<1)|((b->p[b->at>>3]>>(7-(b->at&7)))&1);b->at++;}return v;}
static unsigned ue(bits*b){unsigned z=0;while(!b->bad&&!getbits(b,1)){if(++z>30){b->bad=1;return 0;}}return b->bad?0:((1u<<z)-1)+getbits(b,z);}
/* The encoder contract is Main, 8-bit 4:2:0, with no B-frame reordering.
 * Copy actual profile/tier/compatibility/constraints/level from the SPS PTL.
 * Parse enough SPS to check the displayed geometry and sample format. */
static int hevc_sps(live_mov*m){
 uint8_t rbsp[sizeof m->sps];unsigned n=0,zeros=0;
 for(unsigned i=2;i<m->sps_n;i++){unsigned v=m->sps[i];if(zeros==2&&v==3){if(i+1>=m->sps_n||m->sps[i+1]>3)return -1;zeros=0;continue;}rbsp[n++]=v;zeros=v==0?zeros+1:0;}
 bits b={rbsp,n*8,0,0};getbits(&b,4);unsigned sub=getbits(&b,3),nested=getbits(&b,1);if(sub>6||n<13)return -1;
 uint8_t*c=m->hevc_config;memset(c,0,23);c[0]=1;memcpy(c+1,rbsp+1,12);b.at+=96;
 unsigned profiles[7]={0},levels[7]={0};for(unsigned i=0;i<sub;i++){profiles[i]=getbits(&b,1);levels[i]=getbits(&b,1);}if(sub)for(unsigned i=sub;i<8;i++)getbits(&b,2);
 for(unsigned i=0;i<sub;i++){if(profiles[i]){getbits(&b,32);getbits(&b,32);getbits(&b,24);}if(levels[i])getbits(&b,8);}
 ue(&b);unsigned chroma=ue(&b);if(chroma==3)getbits(&b,1);unsigned width=ue(&b),height=ue(&b),left=0,right=0,top=0,bottom=0;
 if(getbits(&b,1)){left=ue(&b);right=ue(&b);top=ue(&b);bottom=ue(&b);}unsigned luma=ue(&b),chroma_depth=ue(&b);
 if(b.bad||chroma!=1||luma||chroma_depth||left>width/2||right>width/2||top>height/2||bottom>height/2||2*(left+right)>width||2*(top+bottom)>height||((width-2*(left+right)!=m->width||height-2*(top+bottom)!=m->height)&&(left|right|top|bottom||width<m->width||height<m->height||width-m->width>=16||height-m->height>=16)))return -1; /* uncropped coded size: container gives the display size, as for AVC */
 c[13]=0xf0;c[14]=0;c[15]=0xfc;c[16]=0xfc|chroma;c[17]=0xf8|luma;c[18]=0xf8|chroma_depth;c[21]=((sub+1)<<3)|(nested<<2)|3;c[22]=3;return 0;
}
static size_t sc(const uint8_t*p,size_t n,size_t a,unsigned *sz){for(;a+3<=n;a++){if(p[a]==0&&p[a+1]==0){if(p[a+2]==1){*sz=3;return a;}if(a+4<=n&&p[a+2]==0&&p[a+3]==1){*sz=4;return a;}}}*sz=0;return n;}
int live_mov_add(live_mov*m,const uint8_t*p,size_t n,uint64_t pts){
 unsigned z,z2,i=m->count;size_t a,b,start;uint8_t len[4];int vcl=0;
 if(m->failed||!p||i>=LIVE_MAX_FRAMES||(i&&pts<=m->pts[i-1]))return -1;
 if(i&&pts-m->pts[i-1]>0xffffffffu)return -1;
 m->offsets[i]=m->pos;m->pts[i]=pts;a=sc(p,n,0,&z);if(a!=0)return -1;
 /* Nikon's reader splits a chunk at the AUD before each picture. Normalize to
  * exactly one delimiter; parameter sets stay in avcC/hvcC. */
 unsigned hevc_au=m->codec==LIVE_CODEC_HEVC,random=0,found=0,scan_z=z,scan_z2;
 size_t scan_a=a;
 while(scan_z){size_t ss=scan_a+scan_z,ee=sc(p,n,ss,&scan_z2);
  if(ee<=ss||(hevc_au&&ee-ss<2))return -1;
  unsigned ty=hevc_au?(p[ss]>>1)&63:p[ss]&31;
  if(hevc_au?ty<=31:(ty==1||ty==5))found=1;
  if(hevc_au?(ty>=16&&ty<=21):ty==5)random=1;
  scan_a=ee;scan_z=scan_z2;
 }
 if(!found||(!i&&!random))return -1;
 uint8_t aud[3]={hevc_au?0x46:0x09,hevc_au?1:(random?0x10:0x30),random?0x10:0x30};
 put(len,hevc_au?3:2);if(append(m,len,4)||append(m,aud,hevc_au?3:2))return -1;
 while(z){start=a+z;b=sc(p,n,start,&z2);size_t bytes=b-start;while(bytes&&p[start+bytes-1]==0)bytes--;
 if(!bytes)return -1;unsigned hevc=m->codec==LIVE_CODEC_HEVC;if(hevc&&(bytes<2||(p[start]&128)||!(p[start+1]&7)))return -1;unsigned type=hevc?(p[start]>>1)&63:p[start]&31;
 unsigned is_sps=type==(hevc?33u:7u),is_pps=type==(hevc?34u:8u),is_vps=hevc&&type==32;
 if(is_sps||is_pps||is_vps){uint8_t *d=is_sps?m->sps:is_pps?m->pps:m->vps;unsigned *dn=is_sps?&m->sps_n:is_pps?&m->pps_n:&m->vps_n;
 size_t dcap=is_sps?sizeof m->sps:is_pps?sizeof m->pps:sizeof m->vps;if(bytes>dcap||(*dn&&(*dn!=bytes||memcmp(d,p+start,bytes))))return -1;memcpy(d,p+start,bytes);*dn=bytes;
 }else if(type!=(hevc?35u:9u)){if(hevc?(type>=16&&type<=21):type==5)m->sync[i]=1;if(hevc?type<=31:(type==1||type==5))vcl=1;put(len,bytes);if(append(m,len,4)||append(m,p+start,bytes))return -1;}
 a=b;z=z2;
 }
 if(!vcl||(!i&&!m->sync[0])){m->failed=1;return -1;}
 m->sizes[i]=m->pos-m->offsets[i];if(i)m->duration[i-1]=pts-m->pts[i-1];m->count++;return 0;
}
static uint8_t *matrix_o(uint8_t*p,unsigned o,unsigned w,unsigned h){uint32_t a[9]={65536,0,0,0,65536,0,0,0,0x40000000};
 /* v13: display rotation as in iPhone portrait clips; tx/ty 16.16 */
 if(o==6){a[0]=0;a[1]=65536;a[3]=0xffff0000u;a[4]=0;a[6]=h<<16;}
 else if(o==8){a[0]=0;a[1]=0xffff0000u;a[3]=65536;a[4]=0;a[7]=w<<16;}
 else if(o==3){a[0]=0xffff0000u;a[4]=0xffff0000u;a[6]=w<<16;a[7]=h<<16;}
 for(unsigned i=0;i<9;i++)p=put(p,a[i]);return p;}
static uint8_t *matrix(uint8_t*p){return matrix_o(p,1,0,0);}
static uint8_t *handler(uint8_t*p,uint32_t type){uint8_t*a=p;p=F(p,('h','d','l','r'));p=zero(p,4);p=put(p,type);p=zero(p,13);return end(a,p);}
static uint8_t *dinf(uint8_t*p){uint8_t*a=p,*b;p=B(p,('d','i','n','f'));b=p;p=F(p,('d','r','e','f'));p=put(p,1);p=put(p,12);p=put(p,TAG('u','r','l',' '));p=put(p,1);p=end(b,p);return end(a,p);}
static uint8_t *track_header_o(uint8_t*p,unsigned id,unsigned duration,unsigned width,unsigned height,unsigned o){uint8_t*a=p;p=F(p,('t','k','h','d'));a[11]=3;p=zero(p,8);p=put(p,id);p=zero(p,4);p=put(p,duration);p=zero(p,16);p=matrix_o(p,o,width,height);p=put(p,width<<16);p=put(p,height<<16);return end(a,p);}
static uint8_t *track_header(uint8_t*p,unsigned id,unsigned duration,unsigned width,unsigned height){return track_header_o(p,id,duration,width,height,1);}
static uint8_t *media_header(uint8_t*p,unsigned duration,unsigned scale){uint8_t*a=p;p=F(p,('m','d','h','d'));p=zero(p,8);p=put(p,scale);p=put(p,duration);p=put(p,0x55c40000);return end(a,p);}
static uint8_t *tables(uint8_t*p,unsigned count,const uint32_t *duration,const uint32_t *sizes,const uint32_t *offsets,const uint8_t *sync,unsigned period){uint8_t*a=p;p=F(p,('s','t','t','s'));p=put(p,period?1:count);if(period){p=put(p,count);p=put(p,period);}else for(unsigned i=0;i<count;i++){p=put(p,1);p=put(p,duration[i]);}p=end(a,p);
 unsigned starts[LIVE_MAX_FRAMES],samples[LIVE_MAX_FRAMES],chunks=0;
 if(sync){for(unsigned i=0;i<count;i++)if(!i||sync[i])starts[chunks++]=i;}
 else {for(unsigned i=0;i<count;i++)starts[chunks++]=i;}
 for(unsigned i=0;i<chunks;i++)samples[i]=(i+1<chunks?starts[i+1]:count)-starts[i];
 unsigned entries=0;for(unsigned i=0;i<chunks;i++)if(!i||samples[i]!=samples[i-1]||(sync&&i+1==chunks))entries++;
 a=p;p=F(p,('s','t','s','c'));p=put(p,entries);
 for(unsigned i=0;i<chunks;i++)if(!i||samples[i]!=samples[i-1]||(sync&&i+1==chunks)){
  p=put(p,i+1);p=put(p,samples[i]);p=put(p,1);}
 p=end(a,p);
 a=p;p=F(p,('s','t','s','z'));p=put(p,0);p=put(p,count);for(unsigned i=0;i<count;i++)p=put(p,sizes[i]);p=end(a,p);
 a=p;p=F(p,('s','t','c','o'));p=put(p,chunks);for(unsigned i=0;i<chunks;i++)p=put(p,offsets[starts[i]]);return end(a,p);}
/* The capture clock is quantized to integer microseconds. Only select the
 * native 30000/1001 representation when EVERY timestamp and the last edge
 * match that grid within one microsecond; legacy VFR remains unchanged. */
static int is_cfr(const live_mov*m,unsigned duration){
 for(unsigned i=1;i<m->count;i++){
  uint64_t a=(m->pts[i]-m->pts[0])*30u,b=(uint64_t)i*1001000u;
  if(a>b?a-b>=30u:b-a>=30u)return 0;
 }
 uint64_t a=(uint64_t)duration*30u,b=(uint64_t)m->count*1001000u;
 return (a>b?a-b:b-a)<30u;
}
static uint8_t *video(live_mov*m,uint8_t*p,unsigned duration){unsigned cfr=is_cfr(m,duration);uint8_t*t=p,*a,*b,*s,*d,*e;p=B(p,('t','r','a','k'));p=track_header_o(p,1,duration,m->width,m->height,m->orient);a=p;p=B(p,('m','d','i','a'));p=media_header(p,cfr?m->count*1001u:duration,cfr?30000u:1000000u);p=handler(p,TAG('v','i','d','e'));b=p;p=B(p,('m','i','n','f'));d=p;p=F(p,('v','m','h','d'));d[11]=1;p=zero(p,8);p=end(d,p);p=dinf(p);s=p;p=B(p,('s','t','b','l'));d=p;p=F(p,('s','t','s','d'));p=put(p,1);e=p;p=box(p,m->codec==LIVE_CODEC_HEVC?TAG('h','v','c','1'):TAG('a','v','c','1'));p=zero(p,6);*p++=0;*p++=1;p=zero(p,16);*p++=m->width>>8;*p++=m->width;*p++=m->height>>8;*p++=m->height;p=put(p,0x480000);p=put(p,0x480000);p=zero(p,4);*p++=0;*p++=1;p=zero(p,32);*p++=0;*p++=24;*p++=255;*p++=255;
 uint8_t*c=p;if(m->codec==LIVE_CODEC_HEVC){p=B(p,('h','v','c','C'));p=copy(p,m->hevc_config,23);const uint8_t*sets[3]={m->vps,m->sps,m->pps};unsigned sizes[3]={m->vps_n,m->sps_n,m->pps_n};for(unsigned i=0;i<3;i++){*p++=0x80|32|i;*p++=0;*p++=1;*p++=sizes[i]>>8;*p++=sizes[i];p=copy(p,sets[i],sizes[i]);}}else{p=B(p,('a','v','c','C'));*p++=1;*p++=m->sps[1];*p++=m->sps[2];*p++=m->sps[3];*p++=255;*p++=225;*p++=m->sps_n>>8;*p++=m->sps_n;p=copy(p,m->sps,m->sps_n);*p++=1;*p++=m->pps_n>>8;*p++=m->pps_n;p=copy(p,m->pps,m->pps_n);}p=end(c,p);p=end(e,p);p=end(d,p);
 p=tables(p,m->count,m->duration,m->sizes,m->offsets,m->sync,cfr?1001u:0);d=p;p=F(p,('s','t','s','s'));unsigned sync=0;for(unsigned i=0;i<m->count;i++)sync+=m->sync[i];p=put(p,sync);for(unsigned i=0;i<m->count;i++)if(m->sync[i])p=put(p,i+1);p=end(d,p);p=end(s,p);p=end(b,p);p=end(a,p);return end(t,p);}
static uint8_t *metadata(uint8_t*p,const char*uuid,unsigned start,unsigned delta,unsigned offset){
 static const char content[]="com.apple.quicktime.content.identifier",key[]="com.apple.quicktime.still-image-time";
 uint8_t*a=p,*b,*c,*d,*e,*t,*s;p=B(p,('m','e','t','a'));p=handler(p,TAG('m','d','t','a'));b=p;p=F(p,('k','e','y','s'));p=put(p,1);c=p;p=B(p,('m','d','t','a'));p=copy(p,content,sizeof(content)-1);p=end(c,p);p=end(b,p);b=p;p=B(p,('i','l','s','t'));c=p;p=box(p,1);d=p;p=B(p,('d','a','t','a'));p=put(p,1);p=put(p,0);p=copy(p,uuid,36);p=end(d,p);p=end(c,p);p=end(b,p);p=end(a,p);
 t=p;p=B(p,('t','r','a','k'));p=track_header(p,2,start+delta,0,0);a=p;p=B(p,('t','r','e','f'));b=p;p=B(p,('c','d','s','c'));p=put(p,1);p=end(b,p);p=end(a,p);
 a=p;p=B(p,('e','d','t','s'));b=p;p=F(p,('e','l','s','t'));p=put(p,start?2:1);if(start){p=put(p,start);p=put(p,0xffffffff);p=put(p,65536);}p=put(p,delta);p=put(p,0);p=put(p,65536);p=end(b,p);p=end(a,p);
 a=p;p=B(p,('m','d','i','a'));p=media_header(p,delta,1000000u);p=handler(p,TAG('m','e','t','a'));b=p;p=B(p,('m','i','n','f'));c=p;p=F(p,('n','m','h','d'));p=end(c,p);p=dinf(p);s=p;p=B(p,('s','t','b','l'));c=p;p=F(p,('s','t','s','d'));p=put(p,1);d=p;p=B(p,('m','e','b','x'));p=zero(p,6);*p++=0;*p++=1;e=p;p=B(p,('k','e','y','s'));uint8_t*f=p;p=box(p,1);uint8_t*g=p;p=B(p,('k','e','y','d'));p=put(p,TAG('m','d','t','a'));p=copy(p,key,sizeof(key)-1);p=end(g,p);g=p;p=B(p,('d','t','y','p'));p=put(p,0);p=put(p,65);p=end(g,p);p=end(f,p);p=end(e,p);p=end(d,p);p=end(c,p);unsigned size=9;p=tables(p,1,&delta,&size,&offset,0,0);p=end(s,p);p=end(b,p);p=end(a,p);return end(t,p);
}
int live_mov_append_raw(live_mov*m,const void*p,size_t n,uint64_t*at){
 if(!m||m->failed||!m->count||!p||!at)return -1;*at=m->pos;return append(m,p,n);
}
int live_mov_finish(live_mov*m,uint64_t press,uint32_t last,const char uuid[36]){
 return live_mov_finish_with(m,press,last,uuid,0,0);
}
static int finish(live_mov*m,uint64_t press,uint32_t last,const char uuid[36],live_mov_trak_fn extra,void*ctx,int prefix){
 if(!m||m->failed||!uuid||!m->count||m->sps_n<4||!m->pps_n||!last)return -1;
 if(m->codec==LIVE_CODEC_HEVC&&(!m->vps_n||hevc_sps(m)))return -1;
 uint64_t total=m->pts[m->count-1]-m->pts[0]+last;if(total>0xffffffffu||press<m->pts[0]||press>=m->pts[0]+total)return -1;
 unsigned i=0;while(i+1<m->count&&m->pts[i+1]<=press)i++;m->duration[m->count-1]=last;
 unsigned start=m->pts[i]-m->pts[0],delta=m->duration[i],off=m->pos;uint8_t sample[9]={0,0,0,9,0,0,0,1,255};if(append(m,sample,9))return -1;
 uint8_t len[4];put(len,m->pos-24);if(writeat(m,24,len,4))return -1;
 m->moov_at=m->pos;
 uint8_t*p=m->atoms,*moov=p,*a;p=B(p,('m','o','o','v'));a=p;p=F(p,('m','v','h','d'));p=zero(p,8);p=put(p,1000000);p=put(p,total);p=put(p,65536);p=put(p,0x01000000);p=zero(p,8);p=matrix(p);p=zero(p,24);p=put(p,extra?4:3);p=end(a,p);p=video(m,p,total);
 if(extra){size_t n=extra(ctx,p,(size_t)(m->atoms+sizeof(m->atoms)-p));if(!n)return -1;p+=n;}
 uint8_t*tail=p;p=metadata(p,uuid,start,delta,off);p=end(moov,p);
 if((size_t)(p-m->atoms)>sizeof(m->atoms))return -1;
 if(!prefix)return append(m,m->atoms,p-m->atoms);
 m->meta_at=(uint32_t)(tail-m->atoms);m->meta_bytes=(uint32_t)(p-tail);
 return append(m,m->atoms,m->meta_at);
}
int live_mov_finish_with(live_mov*m,uint64_t press,uint32_t last,const char uuid[36],live_mov_trak_fn extra,void*ctx){
 return finish(m,press,last,uuid,extra,ctx,0);
}
int live_mov_finish_prefix(live_mov*m,uint64_t press,uint32_t last,const char uuid[36],live_mov_trak_fn extra,void*ctx){
 return finish(m,press,last,uuid,extra,ctx,1);
}

int live_mov_extend_moov(live_mov*m,uint64_t end){
 uint8_t n[4];if(!m||m->failed||!m->moov_at||end<m->pos||end>UINT32_MAX)return -1;
 if(m->meta_bytes){
  if(m->meta_at>sizeof m->atoms||m->meta_bytes>sizeof m->atoms-m->meta_at||m->meta_bytes>UINT32_MAX-end)return -1;
  m->pos=(uint32_t)end;
  if(append(m,m->atoms+m->meta_at,m->meta_bytes))return -1;
  m->meta_bytes=0;end=m->pos;
 }
 put(n,(uint32_t)end-m->moov_at);if(writeat(m,m->moov_at,n,4))return -1;
 m->pos=(uint32_t)end;return 0;
}
