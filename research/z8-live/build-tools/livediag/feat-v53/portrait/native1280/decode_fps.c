#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#define MiB (1024u*1024u)
typedef struct QEMUTimer QEMUTimer;
static void timer_del(QEMUTimer*t){(void)t;}
static void timer_free(QEMUTimer*t){(void)t;}
#include "nikon-wave5-decode.h"
int main(int argc,char**argv){
 if(argc!=2)return 2;FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);uint8_t*b=malloc(n);if(fread(b,1,n,f)!=n)return 4;fclose(f);
 WlVideoDecode*d=wl_video_dec_open(0);if(!d)return 5;size_t pos=0;unsigned out=0,calls=0;
 while(pos<n){unsigned used=0,avail=MIN(n-pos,4*MiB);bool ok=wl_video_dec_feed(d,b+pos,avail,&used);calls++;pos+=used;
  printf("feed=%u ok=%u consumed=%u pos=%zu queued=%u width=%u height=%u\n",calls,ok,used,pos,g_queue_get_length(&d->frames),d->width,d->height);
  if(!ok)return 6;if(!used&&g_queue_is_empty(&d->frames))return 7;
  while(!g_queue_is_empty(&d->frames)){AVFrame*p=g_queue_pop_head(&d->frames);printf("frame=%u pts=%ld type=%u key=%u\n",out++,p->pts,p->pict_type,!!(p->flags&AV_FRAME_FLAG_KEY));av_frame_free(&p);}
 }
 bool ok=wl_video_dec_end(d);while(!g_queue_is_empty(&d->frames)){AVFrame*p=g_queue_pop_head(&d->frames);printf("frame=%u pts=%ld type=%u key=%u\n",out++,p->pts,p->pict_type,!!(p->flags&AV_FRAME_FLAG_KEY));av_frame_free(&p);}
 printf("done ok=%u frames=%u failed=%u consumed=%zu/%zu\n",ok,out,d->failed,pos,n);printf("fps=%u/%u context_rate=%d/%d\n",d->fps_n,d->fps_d,d->context->framerate.num,d->context->framerate.den);wl_video_dec_close(d);free(b);return !ok||out!=90;
}
