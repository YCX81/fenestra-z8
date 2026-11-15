#include "live_transaction.h"
#include <string.h>
#include "../capture/live_capture.h"
#include "preview_writer.h"
static int sep(char c){return c=='/' || c=='\\';}
/* <drive><sep><top><sep><DCF folder><sep><stem><ext>, keeping the source separator. */
static int layout(const char*source,const char*top,size_t topn,const char*ext,char out[128]){
 if(!source||!out)return -1;
 size_t n=0,base=0,folder=0;while(source[n]){if(n>=90)return -1;if(sep(source[n])){folder=base;base=n+1;}n++;}
 if(n-base!=12||source[base+8]!='.'||base-folder!=9||folder<6)return -1;
 for(unsigned i=0;i<8;i++){char c=source[base+i];if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return -1;}
 size_t drive=0;while(drive<n && !sep(source[drive]))drive++;
 if(drive==n || drive>3)return -1;
 char delimiter=source[drive];char *p=out;memcpy(p,source,drive);p+=drive;*p++=delimiter;memcpy(p,top,topn);p+=topn;*p++=delimiter;
 memcpy(p,source+folder,8);p+=8;*p++=delimiter;memcpy(p,source+base,8);p+=8;memcpy(p,ext,5);return 0;
}
int live_paths(const char*source,char jpg[128],char mov[128]){
 if(!jpg||!mov)return -1;
 return layout(source,"LIVE",4,".JPG",jpg)||layout(source,"LIVE",4,".MOV",mov)?-1:0;
}
int live_data_paths(const char*source,char man[128],char lvp[128]){
 if(!man||!lvp)return -1;
 return layout(source,"LIVEDATA",8,".MAN",man)||layout(source,"LIVEDATA",8,".LVP",lvp)?-1:0;
}
static size_t decimal(char *p,uint64_t n){char b[24];size_t i=0,j=0;do{b[i++]='0'+n%10;n/=10;}while(n);while(i)p[j++]=b[--i];return j;}
static int seek(const live_file_ops*f,int fd,uint64_t at){uint64_t actual=~at;return f->seek(f->opaque,fd,at,&actual)||actual!=at;}
static int readat(const live_file_ops*f,int fd,uint64_t at,void*p,size_t n){size_t got=0;return seek(f,fd,at)||f->read(f->opaque,fd,p,n,&got)||got!=n;}
static int writeat(const live_file_ops*f,int fd,uint64_t at,const void*p,size_t n){size_t got=0;return seek(f,fd,at)||f->write(f->opaque,fd,p,n,&got)||got!=n;}
typedef struct {const live_file_ops*f;int fd;} sink;
static int mov_write(void*v,uint64_t at,const void*p,size_t n){sink*s=v;return writeat(s->f,s->fd,at,p,n);}
static int stop(const live_file_ops*f){return f->cancelled&&f->cancelled(f->opaque);}
static void yield(const live_file_ops*f){if(f->yield)f->yield(f->opaque);}
/* v37: preview pictures are appended to the MOV itself (codec/preview_writer.h). */
typedef struct {const live_file_ops*f;int fd;const live_still_ticket*t;} preview_sink;
static int preview_frame(void*v,uint32_t i,void*p,size_t n){preview_sink*s=v;if(stop(s->f))return -1;return s->t->preview_frame(s->t->preview_ctx,i,p,n);}
static int preview_put(void*v,uint64_t at,const void*p,size_t n){preview_sink*s=v;if(stop(s->f))return -1;return writeat(s->f,s->fd,at,p,n);}
static void preview_yield(void*v){preview_sink*s=v;yield(s->f);}
typedef struct {const live_still_ticket*t;uint64_t at;} audio_sink;
static size_t audio_trak(void*v,uint8_t*p,size_t cap){audio_sink*s=v;return s->t->audio_trak(s->t->audio_ctx,p,cap,s->at);}
/* PCM goes into mdat after the video; on any failure the bytes stay unreferenced. */
static int write_audio(live_transaction_workspace*w,const live_file_ops*f,const live_still_ticket*t,audio_sink*s){
 if(!t->audio_get||!t->audio_trak||!t->audio_frames)return -1;
 uint32_t done=0,chunk=(uint32_t)(sizeof(w->io)/6u);
 while(done<t->audio_frames){
  uint32_t n=t->audio_frames-done<chunk?t->audio_frames-done:chunk,got=0;uint64_t at;
  if(stop(f)||t->audio_get(t->audio_ctx,done,(int16_t*)(void*)w->io,n,&got)||got!=n)return -1;
  /* Expand backwards in the existing I/O buffer: no extra allocation. */
  for(uint32_t k=n*2u;k>0;k--){uint8_t lo=w->io[2u*(k-1)],hi=w->io[2u*(k-1)+1];
   w->io[3u*(k-1)]=0;w->io[3u*(k-1)+1]=lo;w->io[3u*(k-1)+2]=hi;}
  if(live_mov_append_raw(&w->movie,w->io,(size_t)n*6u,&at))return -1;
  if(!done)s->at=at;done+=n;yield(f);
 }
 return 0;
}
static __attribute__((minsize)) int write_pair(live_transaction_workspace*w,const live_file_ops*f,const live_still_ticket*t,const live_input_frame*frames,unsigned count,uint32_t last,void*p,void*s,unsigned*cleanup,const live_input_au *aus,unsigned width,unsigned height){
 char jpg[128],mov[128],manifest[128],prev[128];int in=-1,out=-1,video=-1,mf=-1,ownjpg=0,ownmov=0,ownmf=0,encoding=0,rc=-1;sink sink_;
 if(cleanup)*cleanup=0;
 if(!w||!f||!t||(!frames&&!aus)||!count||count>LIVE_MAX_FRAMES||!last||!cleanup||!t->closed_and_registered||!t->native_path||!f->open_read||!f->create_exclusive||!f->seek||!f->read||!f->write||!f->close||!f->remove_owned||live_paths(t->native_path,jpg,mov))return -1;
 if(stop(f))return -2;
 if(!f->ensure_dir||live_data_paths(t->native_path,manifest,prev))return -1;
 /* Parents of both file sets: LIVE, LIVE\folder, LIVEDATA, LIVEDATA\folder. */
 char *parents[2]={jpg,manifest};
 for(unsigned k=0;k<2;k++){char *q=parents[k];size_t root=0;while(q[root])root++;while(root && !sep(q[root-1]))root--;
  for(size_t i=0;i<root;i++)if(sep(q[i]) && i && !(i==2 && q[1]==':')){
   char save=q[i];q[i]=0;int er=f->ensure_dir(f->opaque,q);q[i]=save;if(er)goto done;
  }}
 unsigned orient=1;
 if(t->has_jpeg){
 if(t->native_size<4||f->open_read(f->opaque,t->native_path,&in))goto done;
 size_t n=t->native_size<sizeof(w->io)?t->native_size:sizeof(w->io);
 if(readat(f,in,0,w->io,n))goto done;
 orient=live_jpeg_orientation(w->io,n);if(t->orient_out)*t->orient_out=orient;
 if(live_jpeg_pair_prefix(w->io,n,t->uuid))goto done;
 if(f->create_exclusive(f->opaque,jpg,&out))goto done;ownjpg=1;
 if(writeat(f,out,0,w->io,n))goto done;uint64_t at=n;
 while(at<t->native_size){if(stop(f)){rc=-2;goto done;}n=t->native_size-at<sizeof(w->io)?t->native_size-at:sizeof(w->io);if(readat(f,in,at,w->io,n)||writeat(f,out,at,w->io,n))goto done;at+=n;yield(f);}
 int r=f->close(f->opaque,in);in=-1;if(r)goto done;r=f->close(f->opaque,out);out=-1;if(r)goto done;
 if(f->encode_event)f->encode_event(f->opaque,10);
 }
 if(stop(f)){rc=-2;goto done;}
 if((!aus && live_encoder_init(&w->encoder,p,s,width,height))||f->create_exclusive(f->opaque,mov,&video))goto done;ownmov=1;sink_.f=f;sink_.fd=video;
 if(live_mov_begin_codec(&w->movie,mov_write,&sink_,width,height,(live_codec_type)live_encoder_codec()))goto done;
 w->movie.orient=orient;
 encoding=1;if(f->encode_event)f->encode_event(f->opaque,0);
 for(unsigned i=0;i<count;i++){const uint8_t*au;size_t n;if(stop(f)){rc=-2;goto done;}uint64_t pts;if(aus){au=aus[i].annexb;n=aus[i].bytes;pts=aus[i].pts_us;}else{if(live_encoder_frame(&w->encoder,frames[i].i420,&au,&n))goto done;pts=frames[i].pts_us;}if(live_mov_add(&w->movie,au,n,pts))goto done;if(f->encode_event)f->encode_event(f->opaque,1);yield(f);}
 if(stop(f)){rc=-2;goto done;}
 audio_sink as={t,0};int audio=t->audio_frames && !write_audio(w,f,t,&as);
 if(w->movie.failed)goto done; /* a failed raw write leaves the MOV unusable */
 if(stop(f)){rc=-2;goto done;}
 /* Keep timed Apple metadata last: the native parser carries its track kind
  * until the next trak. A following udta after an audio-less meta trak is rejected. */
 if(t->preview_frame){if(live_mov_finish_prefix(&w->movie,t->press_us,last,t->uuid,audio?audio_trak:0,&as))goto done;}
 else if(live_mov_finish_with(&w->movie,t->press_us,last,t->uuid,audio?audio_trak:0,&as))goto done;
 /* Four current-clip JPEGs and a small factual NCTG are streamed after the
  * small moov tables, then the moov length is extended. No 16 KiB atom-buffer overflow. */
 if(t->preview_frame){
  if(!t->preview_scratch)goto done;
  preview_sink ns={f,video,t};LivePreviewWriter nw={&ns,preview_frame,preview_put,preview_yield};uint64_t ne=0;
  LiveNikonFacts facts={width,height,w->movie.count,LIVE_W5_FPS_NUM,LIVE_W5_FPS_DEN,
     w->movie.codec==LIVE_CODEC_HEVC?3u:2u,audio?1u:0u};
  if(live_nikon_metadata_write(&nw,t->preview_scratch,LIVE_PREVIEW_FRAME_BYTES,w->io,sizeof(w->io),
     w->movie.pos,&ne,&facts)||stop(f)||live_mov_extend_moov(&w->movie,ne))goto done;
 }
 if(f->encode_event)f->encode_event(f->opaque,11);
 uint64_t mov_end=w->movie.pos;
 if(t->preview_frame){
  /* After moov, in the same file: one free atom every MOV reader skips. A failure
   * removes the whole MOV below, so a partial atom is never left on the card. */
  if(stop(f)){rc=-2;goto done;}
  if(!t->preview_scratch)goto done;
  preview_sink ps={f,video,t};LivePreviewWriter pw={&ps,preview_frame,preview_put,preview_yield};uint64_t end=0;
  if(live_preview_write(&pw,t->preview_scratch,LIVE_PREVIEW_FRAME_BYTES,w->io,sizeof(w->io),w->movie.pos,&end,
     audio&&as.at<=0xffffffffu?(uint32_t)as.at:0u,audio?t->audio_frames:0u,
     (uint32_t)(w->movie.pts[count-1]-w->movie.pts[0]+last)))goto done;
  mov_end=end;
  if(f->encode_event)f->encode_event(f->opaque,12);
 }
 int r=f->close(f->opaque,video);video=-1;if(r)goto done;
 if(t->mov_bytes_out)*t->mov_bytes_out=mov_end;
 if(stop(f)){rc=-2;goto done;}
 if(f->create_exclusive(f->opaque,manifest,&mf))goto done;ownmf=1;
 char *m=(char*)w->io;size_t z=0;const char *keys[] = {"native=","\nsize=","\nuuid=","\nvolume_generation="};
 for(unsigned k=0;k<4;k++){size_t j=0;while(keys[k][j])m[z++]=keys[k][j++];if(k==0){j=0;while(t->native_path[j])m[z++]=t->native_path[j++];}else if(k==1)z+=decimal(m+z,t->native_size);else if(k==2){for(j=0;j<36;j++)m[z++]=t->uuid[j];}else if(k==3)z+=decimal(m+z,t->volume_generation);else z+=decimal(m+z,orient);}
 m[z++]='\n';if(writeat(f,mf,0,m,z))goto done;r=f->close(f->opaque,mf);mf=-1;if(r)goto done;rc=0;
 if(f->encode_event)f->encode_event(f->opaque,13);
 done:
 if(encoding&&f->encode_event)f->encode_event(f->opaque,2);
 if(in>=0&&f->close(f->opaque,in))rc=-1;
 if(out>=0&&f->close(f->opaque,out))rc=-1;
 if(video>=0&&f->close(f->opaque,video))rc=-1;
 if(mf>=0&&f->close(f->opaque,mf))rc=-1;
 if(rc){if(ownmf&&f->remove_owned(f->opaque,manifest))*cleanup|=4;if(ownjpg&&f->remove_owned(f->opaque,jpg))*cleanup|=1;if(ownmov&&f->remove_owned(f->opaque,mov))*cleanup|=2;}
 return rc;
}

/* Backup / second-card copy: the same Live set for the native JPG on the other card.
 * LIVE JPG is rebuilt from THAT card's native JPG (JPEG+JPEG slot roles may differ in
 * size), the MOV is copied from the first card, the manifest names the twin. */
static size_t drive_len(const char*p){size_t i=0;while(p[i]&&!sep(p[i]))i++;return p[i]?i:0;}
static int differ(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a!=*b;}
static int ensure_parents(const live_file_ops*f,char*q){
 size_t root=0;while(q[root])root++;while(root && !sep(q[root-1]))root--;
 for(size_t i=0;i<root;i++)if(sep(q[i]) && i && !(i==2 && q[1]==':')){
  char save=q[i];q[i]=0;int er=f->ensure_dir(f->opaque,q);q[i]=save;if(er)return -1;}
 return 0;
}
int live_write_twin(live_transaction_workspace*w,const live_file_ops*f,const char*source_native,uint64_t mov_size,
 const char*twin_native,uint64_t twin_size,const char uuid[36],unsigned volume_generation,unsigned*cleanup){
 char sjpg[128],smov[128],tjpg[128],tmov[128],tman[128],tlvp[128];int in=-1,out=-1,ownjpg=0,ownmov=0,ownmf=0,rc=-1;
 if(cleanup)*cleanup=0;
 if(!w||!f||!source_native||!twin_native||!uuid||!cleanup||!f->open_read||!f->create_exclusive||!f->seek||!f->read||
    !f->write||!f->close||!f->remove_owned||!f->ensure_dir)return -1;
 if(live_paths(source_native,sjpg,smov)||live_paths(twin_native,tjpg,tmov)||live_data_paths(twin_native,tman,tlvp))return -1;
 size_t a=drive_len(sjpg),b=drive_len(tjpg);
 if(!a||a!=b||!memcmp(sjpg,tjpg,a)||differ(sjpg+a,tjpg+b))return -1;  /* same names, other volume only */
 if(twin_size<4||mov_size<8)return -1;
 if(stop(f))return -2;
 if(ensure_parents(f,tjpg)||ensure_parents(f,tman))return -1;
 /* 1. importable JPG of the twin, with the same pairing identity as the MOV */
 if(f->open_read(f->opaque,twin_native,&in))goto done;
 size_t n=twin_size<sizeof(w->io)?(size_t)twin_size:sizeof(w->io);
 if(readat(f,in,0,w->io,n)||live_jpeg_pair_prefix(w->io,n,uuid))goto done;
 if(f->create_exclusive(f->opaque,tjpg,&out))goto done;ownjpg=1;
 if(writeat(f,out,0,w->io,n))goto done;uint64_t at=n;
 while(at<twin_size){if(stop(f)){rc=-2;goto done;}n=twin_size-at<sizeof(w->io)?(size_t)(twin_size-at):sizeof(w->io);
  if(readat(f,in,at,w->io,n)||writeat(f,out,at,w->io,n))goto done;at+=n;yield(f);}
 int r=f->close(f->opaque,in);in=-1;if(r)goto done;r=f->close(f->opaque,out);out=-1;if(r)goto done;
 /* 2. MOV, byte copy (video, audio and the in-camera preview atom) */
 if(stop(f)){rc=-2;goto done;}
 if(f->open_read(f->opaque,smov,&in))goto done;
 if(f->create_exclusive(f->opaque,tmov,&out))goto done;ownmov=1;
 at=0;while(at<mov_size){if(stop(f)){rc=-2;goto done;}n=mov_size-at<sizeof(w->io)?(size_t)(mov_size-at):sizeof(w->io);
  if(readat(f,in,at,w->io,n)||writeat(f,out,at,w->io,n))goto done;at+=n;yield(f);}
 r=f->close(f->opaque,in);in=-1;if(r)goto done;r=f->close(f->opaque,out);out=-1;if(r)goto done;
 /* 3. manifest naming the twin, written last as in write_pair */
 if(stop(f)){rc=-2;goto done;}
 if(f->create_exclusive(f->opaque,tman,&out))goto done;ownmf=1;
 char *m=(char*)w->io;size_t z=0;const char *keys[] = {"native=","\nsize=","\nuuid=","\nvolume_generation="};
 for(unsigned k=0;k<4;k++){size_t j=0;while(keys[k][j])m[z++]=keys[k][j++];
  if(k==0){j=0;while(twin_native[j])m[z++]=twin_native[j++];}else if(k==1)z+=decimal(m+z,twin_size);
  else if(k==2){for(j=0;j<36;j++)m[z++]=uuid[j];}else z+=decimal(m+z,volume_generation);}
 m[z++]='\n';if(writeat(f,out,0,m,z))goto done;r=f->close(f->opaque,out);out=-1;if(r)goto done;rc=0;
 done:
 if(in>=0&&f->close(f->opaque,in))rc=-1;
 if(out>=0&&f->close(f->opaque,out))rc=-1;
 if(rc){if(ownmf&&f->remove_owned(f->opaque,tman))*cleanup|=4;if(ownjpg&&f->remove_owned(f->opaque,tjpg))*cleanup|=1;if(ownmov&&f->remove_owned(f->opaque,tmov))*cleanup|=2;}
 return rc;
}
int live_write_pair(live_transaction_workspace*w,const live_file_ops*f,
 const live_still_ticket*t,const live_input_frame*frames,unsigned count,
 uint32_t last,void*p,void*s,unsigned*cleanup) {
 return write_pair(w,f,t,frames,count,last,p,s,cleanup,0,LA_WIDTH,LA_HEIGHT);
}
int live_write_pair_aus(live_transaction_workspace*w,const live_file_ops*f,
 const live_still_ticket*t,const live_input_au*aus,unsigned count,unsigned width,
 unsigned height,uint32_t last,unsigned*cleanup) {
 return write_pair(w,f,t,0,count,last,0,0,cleanup,aus,width,height);
}
