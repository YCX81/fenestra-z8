#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define g_malloc malloc
#define g_free free
#include "/path/to/user/fxfix/mvq/src/hw/arm/nikon-z8-jpegdec.h"
int main(int argc,char**argv){for(int i=1;i<argc;i++){FILE*f=fopen(argv[i],"rb");if(!f)return 1;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);uint8_t*d=malloc(n);if(fread(d,1,n,f)!=(size_t)n)return 2;fclose(f);unsigned w,h,fmt;bool info=z8_jpegdec_info(d,n,&w,&h,&fmt);if(!info)return 3;printf("%s header %ux%u format=%u ",argv[i],w,h,fmt);uint8_t*p=z8_jpegdec_ycc(d,n,0,0,&w,&h);if(!p)return 4;printf("decode %ux%u PASS\n",w,h);free(p);free(d);}return 0;}
