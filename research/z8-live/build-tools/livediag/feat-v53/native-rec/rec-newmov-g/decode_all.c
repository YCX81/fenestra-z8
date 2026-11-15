#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "preview_reader.h"
int main(int argc,char **argv){if(argc!=2)return 2;FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);uint8_t*b=malloc(n);if(fread(b,1,n,f)!=(size_t)n)return 4;fclose(f);uint64_t at;uint32_t sz;LpClip c;LpPreview p;if(lp_clip_tail(b+n-8,n,&at,&sz)||lp_clip_open(b+at,sz,at,sz,&p,&c,9))return 5;uint8_t*out=malloc(p.frame_bytes);LpJpeg*j=malloc(sizeof(*j));printf("{\"frames\":%u,\"duration_us\":%u,\"decode\":[",p.frames,p.duration_us);for(uint32_t i=0;i<p.frames;i++){int rc=lp_jpeg_decode(j,b+c.at[i],c.at[i+1]-c.at[i],out,p.width,p.height);if(i)printf(",");printf("%d",rc);if(rc)return 6;}puts("]}");free(j);free(out);free(b);return 0;}
