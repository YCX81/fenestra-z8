"""E1: REC on a Live photo enters the stock movie page with id|5; the file
service open of the photo path is redirected to the Live MOV while active."""
import sys
from pathlib import Path
W=Path(sys.argv[1])
def sub(path,old,new,count=1):
    p=W/path;t=p.read_text();assert t.count(old)==count,(path,old[:50],t.count(old));p.write_text(t.replace(old,new))
sub('pb-fix37/runtime.h','    _Atomic uint32_t audio_request_gate;\n} PbRuntime;',
'''    _Atomic uint32_t audio_request_gate;
    /* one-MOV: stock movie player bridge */
    _Atomic uint32_t nb_request,nb_stage;
    uint32_t nb_photo,nb_movie,nb_starts,nb_returns,nb_redirects,nb_view,nb_cur,nb_opens;
    char nb_from[64],nb_to[64];
} PbRuntime;''')
R='pb-fix37/runtime.c'
sub(R,'uint32_t pb_ui_poll_wrap(void *flags){',
'''int32_t pb_open_wrap(const char*path,uint32_t a,uint32_t b){
 PbRuntime*r=pb_runtime_get();
 if(r&&LOAD(&r->nb_stage)==2u&&path){r->nb_opens++;if(!memcmp(path,r->nb_from,30)){r->nb_redirects++;path=r->nb_to;}}
 return FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))(path,a,b);
}
uint32_t pb_native_return(uint32_t kind,uint32_t page){
 PbRuntime*r=pb_runtime_get();
 if(r&&kind==3&&page==0x552&&LOAD(&r->nb_stage)==2u){
  STORE(&r->nb_stage,0);r->nb_returns++;
  FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_photo);
 }
 return FN(0x403ad6a4,uint32_t(*)(uint32_t,uint32_t))(kind,page);
}
uint32_t pb_ui_poll_wrap(void *flags){''')
sub(R,''' r->ui_polls++;
 uint32_t request=LOAD(&r->render_request);''',
''' r->ui_polls++;
 if(atomic_exchange_explicit(&r->nb_request,0,memory_order_acq_rel)){
  uint32_t id=0;char to[64];
  if(playback_owner()&&current(&id)&&id==r->current_id&&r->cache_valid&&!(id&15u)&&!live_sync_file(to,r->native_path,LIVE_FILE_MOV)){
   memcpy(r->nb_from,r->native_path,64);memcpy(r->nb_to,to,64);
   r->nb_photo=id;r->nb_movie=id|5u;r->nb_starts++;
   pb_ui_leave();
   FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_movie);
   STORE(&r->nb_stage,2);
   FN(0x406364d0,void(*)(void))();
   FN(0x403fb660,void(*)(uint32_t))(1);
   FN(0x403ad6a4,void(*)(uint32_t,uint32_t))(3,0x56c);
   FN(0x403fb660,void(*)(uint32_t))(0);
   return native_result;
  }
 }
 uint32_t request=LOAD(&r->render_request);''')
# legacy JPEG preview player off: REC always goes to the stock player
sub(R,'if(rec&&owns_rec&&!r->playing){STORE(&r->audio_request_gate,LOAD(&r->audio_gate));STORE(&r->start_request,1);}',
      'if(rec&&owns_rec)STORE(&r->nb_request,1);')
sub(R,'''   if(LOAD(&r->start_request)){
    STORE(&r->start_request,0);
    if(r->cache_valid&&LOAD(&r->key)==9&&!start(r,now_us()))r->blocked_key_epoch=0; /* nothing to play: badge back */
   }
''','')
sub(R,'''   if(r->playing&&(r->audio_pending||(((r->audio_stage>=1u&&r->audio_stage<=3u)||r->audio_stage==7u||r->audio_stage==8u)&&!r->audio_stop)))r->start_us=now_us();
   else if(r->playing)frame(r,now_us());
   else if(r->cache_valid''','''   if(r->cache_valid''')
sub(R,'  audio_step(r);\n','  if(LOAD(&r->nb_stage)==2u){FxNativeView v=pb_native_current_view();r->nb_view=(uint32_t)v.id|(uint32_t)v.state<<8|(uint32_t)active<<16|(v.address==UINT64_C(0xa0284a0e0)?1u<<24:0);r->nb_cur=id;}\n')
# drop now-unused statics
t=(W/R).read_text()
a=t.index('static int read_at(');b=t.index('/* ---- preview sound')
t=t[:a]+t[b:]
a=t.index('static void audio_release(');b=t.index('static void worker(void *arg){')
t=t[:a]+t[b:]
(W/R).write_text(t)
print('patched')
