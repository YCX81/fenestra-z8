#include "live_hevc.h"
#include "live_codec.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
static int wr(void*o,uint64_t at,const void*d,size_t n){FILE*f=o;fseek(f,(long)at,SEEK_SET);return fwrite(d,1,n,f)!=n;}
int main(int c,char**v){
 setvbuf(stdout,0,_IONBF,0);static uint8_t b[4096];FILE*f=fopen(v[1],"rb");size_t n=fread(b,1,sizeof b,f);fclose(f);
 size_t idx[16];int k=0;for(size_t i=0;i+4<n&&k<16;i++)if(!b[i]&&!b[i+1]&&!b[i+2]&&b[i+3]==1)idx[k++]=i+4;
 static live_hevc h;memset(&h,0,sizeof h);
 for(int i=0;i<4;i++){size_t len=i<3?idx[i+1]-idx[i]-4:7;int t=(b[idx[i]]>>1)&63;int r=live_hevc_param(&h,b+idx[i],len,1024,682);printf("nal type %d len %zu -> %d\n",t,len,r);assert(!r);}
 printf("ready %u cropped %u vps %u sps %u (raw %u) pps %u\n",h.ready,h.cropped,h.vps_n,h.sps_n,h.sps_raw_n,h.pps_n);
 assert(h.ready&&h.cropped);
 FILE*o=fopen("/tmp/hwps.hevc","wb");const uint8_t sc[4]={0,0,0,1};
 fwrite(sc,1,4,o);fwrite(h.vps,1,h.vps_n,o);fwrite(sc,1,4,o);fwrite(h.sps,1,h.sps_n,o);fwrite(sc,1,4,o);fwrite(h.pps,1,h.pps_n,o);fclose(o);
 /* MOV writer: feed parameter sets as an AU prefix via live_mov_add path is VCL-gated; check its SPS parser directly */
 static live_mov m;FILE*mf=fopen("/tmp/hwps.mov","w+b");assert(!live_mov_begin_codec(&m,wr,mf,1024,682,LIVE_CODEC_HEVC));
 memcpy(m.vps,h.vps,h.vps_n);m.vps_n=h.vps_n;memcpy(m.sps,h.sps,h.sps_n);m.sps_n=h.sps_n;memcpy(m.pps,h.pps,h.pps_n);m.pps_n=h.pps_n;
 extern int live_mov_test_hevc_sps(live_mov*);printf("mov hevc_sps -> %d config %02x %02x %02x\n",live_mov_test_hevc_sps(&m),m.hevc_config[0],m.hevc_config[1],m.hevc_config[12]);
 return 0;}
