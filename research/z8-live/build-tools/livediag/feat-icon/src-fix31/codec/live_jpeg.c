/* SPDX-License-Identifier: GPL-2.0-or-later
 * Apple note payload matches live_hook.c; unlike its NCVW writer this only
 * replaces the native JPEG's existing note, preserving all MPO offsets. */
#include "live_codec.h"
#include <string.h>
static unsigned r16(const uint8_t*p,int le){return le?p[0]|p[1]<<8:p[0]<<8|p[1];}
static uint32_t r32(const uint8_t*p,int le){return le?(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24:(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void w32(uint8_t*p,uint32_t v,int le){for(unsigned i=0;i<4;i++)p[le?i:3-i]=v>>(i*8);}
static uint8_t *entry(uint8_t*t,size_t n,uint32_t at,unsigned tag,int le){
 if(at>n||n-at<2)return 0;unsigned count=r16(t+at,le);if(count>(n-at-2)/12)return 0;
 for(unsigned i=0;i<count;i++){uint8_t*p=t+at+2+12*i;if(r16(p,le)==tag)return p;}return 0;
}
unsigned live_jpeg_orientation(const uint8_t*p,size_t n){
 if(!p||n<4||p[0]!=255||p[1]!=216)return 1;
 for(size_t at=2;at+4<=n;){if(p[at]!=255)return 1;unsigned marker=p[at+1];if(marker==0xda||marker==0xd9)return 1;
  unsigned len=r16(p+at+2,0);if(len<2||len>n-at-2)return 1;
  if(marker==0xe1&&len>=16&&!memcmp(p+at+4,"Exif\0\0",6)){
   uint8_t*t=(uint8_t*)p+at+10;size_t tn=len-8;int le=t[0]=='I'&&t[1]=='I';if(!le&&!(t[0]=='M'&&t[1]=='M'))return 1;
   const uint8_t*e=entry(t,tn,r32(t+4,le),0x0112,le);if(!e||r16(e+2,le)!=3||r32(e+4,le)!=1)return 1;
   unsigned o=r16(e+8,le);return o>=1&&o<=8?o:1;}
  at+=len+2;}
 return 1;
}
int live_jpeg_pair_prefix(uint8_t*p,size_t n,const char uuid[36]){
 if(!p||!uuid||n<4||p[0]!=255||p[1]!=216)return -1;
 for(size_t at=2;at+4<=n;){if(p[at]!=255)return -1;unsigned marker=p[at+1];if(marker==0xda||marker==0xd9)return -1;
 unsigned len=r16(p+at+2,0);if(len<2||len>n-at-2)return -1;
 if(marker==0xe1&&len>=16&&!memcmp(p+at+4,"Exif\0\0",6)){
 uint8_t*t=p+at+10;size_t tn=len-8;int le=t[0]=='I'&&t[1]=='I';if(!le&&!(t[0]=='M'&&t[1]=='M'))return -1;if(r16(t+2,le)!=42)return -1;
 uint8_t*e=entry(t,tn,r32(t+4,le),0x8769,le);if(!e||r16(e+2,le)!=4||r32(e+4,le)!=1)return -1;
 e=entry(t,tn,r32(e+8,le),0x927c,le);if(!e||r16(e+2,le)!=7)return -1;
 uint32_t size=r32(e+4,le),off=r32(e+8,le);if(size<69||off>tn||size>tn-off)return -1;
 uint8_t*note=t+off;memset(note,0,size);memcpy(note,"Apple iOS\0\0\1MM",14);note[15]=1;note[17]=17;note[19]=2;
 w32(note+20,37,0);w32(note+24,32,0);memcpy(note+32,uuid,36);w32(e+4,69,le);return 0;
 }at+=len+2;
 }return -1;
}
