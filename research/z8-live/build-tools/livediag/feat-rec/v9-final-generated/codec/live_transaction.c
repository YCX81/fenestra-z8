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
typedef struct {const live_file_ops*f;int fd;uint64_t at;const live_still_ticket*t;} preview_sink;
static int preview_frame(void*v,uint32_t i,void*p,size_t n){preview_sink*s=v;return s->t->preview_frame(s->t->preview_ctx,i,p,n);}
static int preview_put(void*v,const void*p,size_t n,size_t*done){preview_sink*s=v;if(writeat(s->f,s->fd,s->at,p,n))return -1;s->at+=n;*done=n;return 0;}
static void preview_yield(void*v){preview_sink*s=v;yield(s->f);}
typedef struct {const live_still_ticket*t;uint64_t at;} audio_sink;
static size_t audio_trak(void*v,uint8_t*p,size_t cap){audio_sink*s=v;return s->t->audio_trak(s->t->audio_ctx,p,cap,s->at);}
/* PCM goes into mdat after the video; on any failure the bytes stay unreferenced. */
static int write_audio(live_transaction_workspace*w,const live_file_ops*f,const live_still_ticket*t,audio_sink*s){
 if(!t->audio_get||!t->audio_trak||!t->audio_frames)return -1;
 uint32_t done=0,chunk=(uint32_t)(sizeof(w->io)/4u);
 while(done<t->audio_frames){
  uint32_t n=t->audio_frames-done<chunk?t->audio_frames-done:chunk,got=0;uint64_t at;
  if(stop(f)||t->audio_get(t->audio_ctx,done,(int16_t*)(void*)w->io,n,&got)||got!=n)return -1;
  if(live_mov_append_raw(&w->movie,w->io,(size_t)n*4u,&at))return -1;
  if(!done)s->at=at;done+=n;yield(f);
 }
 return 0;
}
static int write_pair(live_transaction_workspace*w,const live_file_ops*f,const live_still_ticket*t,const live_input_frame*frames,unsigned count,uint32_t last,void*p,void*s,unsigned*cleanup,const live_input_au *aus,unsigned width,unsigned height){
 char jpg[128],mov[128],manifest[128],prev[128];int in=-1,out=-1,video=-1,mf=-1,pv=-1,ownjpg=0,ownmov=0,ownmf=0,ownpv=0,encoding=0,rc=-1;sink sink_;
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
 if(live_mov_begin(&w->movie,mov_write,&sink_,width,height))goto done;
 w->movie.orient=orient;
 encoding=1;if(f->encode_event)f->encode_event(f->opaque,0);
 for(unsigned i=0;i<count;i++){const uint8_t*au;size_t n;if(stop(f)){rc=-2;goto done;}uint64_t pts;if(aus){au=aus[i].annexb;n=aus[i].bytes;pts=aus[i].pts_us;}else{if(live_encoder_frame(&w->encoder,frames[i].i420,&au,&n))goto done;pts=frames[i].pts_us;}if(live_mov_add(&w->movie,au,n,pts))goto done;if(f->encode_event)f->encode_event(f->opaque,1);yield(f);}
 if(stop(f)){rc=-2;goto done;}
 audio_sink as={t,0};int audio=t->audio_frames && !write_audio(w,f,t,&as);
 if(w->movie.failed)goto done; /* a failed raw write leaves the MOV unusable */
 if(stop(f)){rc=-2;goto done;}
 if(live_mov_finish_with(&w->movie,t->press_us,last,t->uuid,audio?audio_trak:0,&as))goto done;
 int r=f->close(f->opaque,video);video=-1;if(r)goto done;
 if(f->encode_event)f->encode_event(f->opaque,11);
 if(t->preview_frame){
  if(stop(f)){rc=-2;goto done;}
  if(!t->preview_scratch||f->create_exclusive(f->opaque,prev,&pv))goto done;ownpv=1;
  preview_sink ps={f,pv,0,t};LivePreviewWriter pw={&ps,preview_frame,preview_put,preview_yield};
  if(live_preview_write(&pw,t->preview_scratch,LIVE_PREVIEW_FRAME_BYTES))goto done;
  r=f->close(f->opaque,pv);pv=-1;if(r)goto done;
  if(f->encode_event)f->encode_event(f->opaque,12);
 }
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
 if(pv>=0&&f->close(f->opaque,pv))rc=-1;
 if(rc){if(ownmf&&f->remove_owned(f->opaque,manifest))*cleanup|=4;if(ownjpg&&f->remove_owned(f->opaque,jpg))*cleanup|=1;if(ownmov&&f->remove_owned(f->opaque,mov))*cleanup|=2;if(ownpv&&f->remove_owned(f->opaque,prev))*cleanup|=8;}
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
