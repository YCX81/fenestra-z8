#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include "preview_reader.h"
int main(int argc,char**argv){assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);uint8_t*b=malloc(n);assert(fread(b,1,n,f)==(size_t)n);fclose(f);uint64_t at;uint32_t bytes;assert(!lp_clip_tail(b+n-8,n,&at,&bytes));LpPreview p;LpClip c;assert(!lp_clip_open(b+at,bytes,at,bytes,&p,&c,1));LpJpeg*j=calloc(1,sizeof(*j));uint8_t*o=malloc(p.frame_bytes);for(unsigned i=0;i<p.frames;i++){int r=lp_jpeg_decode(j,b+c.at[i],c.at[i+1]-c.at[i],o,p.width,p.height);assert(r==0);uint32_t sum=0;for(unsigned k=0;k<p.frame_bytes;k++)sum=sum*31+o[k];printf("%u %llu %08x\n",i,(unsigned long long)(c.at[i+1]-c.at[i]),sum);}free(j);free(o);free(b);return 0;}
