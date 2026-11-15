"""One-MOV playback: REC on a Live photo plays LIVE\\dir\\stem.MOV in the stock
movie page. For the playback window only, the photo's DCF node carries a MOV
member (bit 5) and the stock open/stat of DCIM\\dir\\stem.MOV is redirected."""
import sys
from pathlib import Path
W=Path(sys.argv[1])
def sub(path,old,new,count=1):
    p=W/path;t=p.read_text();assert t.count(old)==count,(path,old[:50],t.count(old));p.write_text(t.replace(old,new))
sub('pb-fix37/runtime.h','    _Atomic uint32_t audio_request_gate;\n} PbRuntime;',
'''    _Atomic uint32_t audio_request_gate;
    /* one-MOV: stock movie player bridge */
    _Atomic uint32_t nb_request,nb_stage;
    uint32_t nb_photo,nb_movie,nb_starts,nb_returns,nb_redirects,nb_view,nb_cur,nb_stats;
    uint32_t nb_w,nb_h,nb_bit,nb_fail;
    uintptr_t nb_node;
    _Alignas(8) uint8_t nb_info[0x48];
    char nb_from[64],nb_to[64];
} PbRuntime;''')
R='pb-fix37/runtime.c'
sub(R,'uint32_t pb_ui_poll_wrap(void *flags){',
r'''/* ---- one-MOV bridge ---- */
__attribute__((noinline)) void pb_nb_observe(const void*state,uint32_t where){__asm__ volatile(""::"r"(state),"r"(where):"memory");}
static int nb_match(PbRuntime*r,const char*path){return r&&LOAD(&r->nb_stage)==2u&&path&&!memcmp(path,r->nb_from,30);}
int32_t pb_open_wrap(const char*path,uint32_t a,uint32_t b){
 PbRuntime*r=pb_runtime_get();
 if(nb_match(r,path)){r->nb_redirects++;path=r->nb_to;}
 return FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))(path,a,b);
}
uint32_t pb_stat_wrap(const char*path,void*out){
 PbRuntime*r=pb_runtime_get();
 if(nb_match(r,path)){r->nb_stats++;path=r->nb_to;}
 return FN(0x402dadf4,uint32_t(*)(const char*,void*))(path,out);
}
/* 40711308 fills the playback UI's clip facts from the DCF record of the ref.
 * Inside a JPG group that record describes the photo. Hand it a copy carrying
 * the clip's facts: 29.97p code 3 (-> 4,2), movie flag, sound class 1, real size. */
void *pb_info_record(void*ref){
 uint8_t*o=FN(0x406bc37c,void*(*)(void*))(ref);PbRuntime*r=pb_runtime_get();
 if(!o||!r||LOAD(&r->nb_stage)!=2u)return o;
 uint8_t*c=r->nb_info;memcpy(c,o,sizeof(r->nb_info));
 c[4]=3u;c[5]|=4u;c[0xb]=(uint8_t)((c[0xb]&~3u)|1u);
 c[0xc]=(uint8_t)r->nb_w;c[0xd]=(uint8_t)(r->nb_w>>8);c[0xe]=(uint8_t)r->nb_h;c[0xf]=(uint8_t)(r->nb_h>>8);
 return c;
}
/* The photo's node in the stock DCF index: the lookup 4085fa38 falls back to
 * (directory 4085e91c, then node 4086c378). Member formats are the u16 at +0x24.
 * Called with the DCF lock held; takes the slot lock the stock readback takes. */
static uint16_t *nb_mask(uint32_t id,int set){
 uint32_t card=id>>29&3u;if(card>1u)return 0;
 uintptr_t slot=((volatile uintptr_t*)(uintptr_t)UINT64_C(0xa04822af8))[card];
 if(slot<UINT64_C(0x800000000)||slot>=UINT64_C(0x1000000000)||(slot&7u))return 0;   /* e149e6dc0 on C3.11 */
 FN(0x40875834,void(*)(uintptr_t,uint32_t,uint32_t))(slot+0x222138u,0x64,0x64);
 uintptr_t dir=FN(0x4085e91c,uintptr_t(*)(uintptr_t,uint32_t))(slot,id>>19&0x3ffu);
 uintptr_t node=dir?FN(0x4086c378,uintptr_t(*)(uintptr_t,uint32_t,void*))(dir,id,0):0;
 uint16_t*m=node>=slot&&node<slot+0x222188u?(uint16_t*)(node+0x24u):0;
 if(m){if(set)*m|=0x20u;else *m&=(uint16_t)~0x20u;}
 FN(0x408758b4,void(*)(uintptr_t))(slot+0x222138u);
 return m;
}
static void nb_end(PbRuntime*r){
 if(LOAD(&r->nb_stage)!=2u)return;
 STORE(&r->nb_stage,0);
 if(r->nb_bit){
  FN(0x4085d348,void(*)(void))();
  nb_mask(r->nb_photo,0);
  FN(0x4085d364,void(*)(void))();r->nb_bit=0;
 }
 pb_nb_observe(&r->nb_request,3);
}
uint32_t pb_native_return(uint32_t kind,uint32_t page){
 PbRuntime*r=pb_runtime_get();
 if(r&&kind==3&&page==0x552&&LOAD(&r->nb_stage)==2u){
  nb_end(r);r->nb_returns++;
  FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_photo);
 }
 return FN(0x403ad6a4,uint32_t(*)(uint32_t,uint32_t))(kind,page);
}
static void nb_start(PbRuntime*r){
 uint32_t id=0;char to[64];
 if(LOAD(&r->nb_stage)||!playback_owner()||!current(&id)||id!=r->current_id||!r->cache_valid||!r->nb_w||
    live_sync_file(to,r->native_path,LIVE_FILE_MOV)){r->nb_fail=1;r->nb_cur=id;pb_nb_observe(&r->nb_request,1);return;}
 memcpy(r->nb_from,r->native_path,64);memcpy(r->nb_from+25,".MOV",5);memcpy(r->nb_to,to,64);
 r->nb_photo=id;r->nb_movie=(id&~15u)|5u;
 FN(0x4085d348,void(*)(void))();
 uint16_t*m=nb_mask(id,1);r->nb_node=(uintptr_t)m;r->nb_bit=m!=0;
 FN(0x4085d364,void(*)(void))();
 if(!m){r->nb_fail=2;pb_nb_observe(&r->nb_request,1);return;}
 r->nb_starts++;r->nb_fail=0;
 pb_ui_leave();
 STORE(&r->nb_stage,2);pb_nb_observe(&r->nb_request,2);
 FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_movie);
 FN(0x406364d0,void(*)(void))();
 FN(0x403fb660,void(*)(uint32_t))(1);
 FN(0x403ad6a4,void(*)(uint32_t,uint32_t))(3,0x56c);
 FN(0x403fb660,void(*)(uint32_t))(0);
}
uint32_t pb_ui_poll_wrap(void *flags){''')
sub(R,''' r->ui_polls++;
 uint32_t request=LOAD(&r->render_request);''',
''' r->ui_polls++;
 if(atomic_exchange_explicit(&r->nb_request,0,memory_order_acq_rel)){nb_start(r);if(LOAD(&r->nb_stage)==2u)return native_result;}
 uint32_t request=LOAD(&r->render_request);''')
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
# clip size: first video tkhd of a niko-brand MOV (the writer's stock-playable layout)
sub(R,' r->photo_size=size;r->lookup_error=0;r->cache_valid=1;\n}',
r''' r->photo_size=size;r->lookup_error=0;r->cache_valid=1;
 mov_size(r);
}''')
sub(R,'static void lookup(PbRuntime*r,uint32_t id){',
r'''static uint32_t be32(const uint8_t*p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void mov_size(PbRuntime*r){
 char p[64];uint8_t h[16];uint64_t at=0,lim=0,pos=0;uint32_t got=0,w=0,hh=0,nikon=0;r->nb_w=r->nb_h=0;
 if(live_sync_file(p,r->native_path,LIVE_FILE_MOV))return;
 int32_t fd=FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))(p,0,0x20);
 if(fd<0||(uint32_t)fd==0x7fff0000)return;
 if(FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,0,2,&lim))
 for(unsigned i=0;i<48u&&at+16u<=lim;i++){
  if(!FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,(int64_t)at,0,&pos)||pos!=at||
     FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(fd,h,16u,&got)||got!=16u)break;
  uint64_t n=be32(h);uint32_t t=be32(h+4),head=8;
  if(n==1u){n=(uint64_t)be32(h+8)<<32|be32(h+12);head=16;}
  if(n<head||n>lim-at)break;
  if(!at&&t!=0x66747970u)break;
  if(t==0x6d6f6f76u){lim=at+n;at+=head;continue;}              /* moov: walk its children */
  if(t==0x75647461u&&n>=4096u)nikon=1;                        /* udta with the stock NCDT block */
  if(t==0x7472616bu&&n>=head+92u&&!w){                               /* trak: tkhd is its first child */
   uint8_t k[24];uint64_t q=at+head;
   if(FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,(int64_t)q,0,&pos)&&pos==q&&
      !FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(fd,k,8u,&got)&&got==8u&&be32(k+4)==0x746b6864u){
    uint64_t e=q+be32(k)-8u;
    if(be32(k)>=92u&&e+8u<=at+n&&FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,(int64_t)e,0,&pos)&&pos==e&&
       !FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(fd,k,8u,&got)&&got==8u){
     w=be32(k)>>16;hh=be32(k+4)>>16;if(!hh)w=0;
    }
   }
  }
  at+=n;
 }
 FN(0x402dd558,uint32_t(*)(int32_t))(fd);
 if(nikon&&w&&w<=1920u&&hh<=1088u){r->nb_w=w;r->nb_h=hh;}
}
static void lookup(PbRuntime*r,uint32_t id){''')
# drop now-unused statics
t=(W/R).read_text()
a=t.index('static int read_at(');b=t.index('/* ---- preview sound')
t=t[:a]+t[b:]
a=t.index('static void audio_release(');b=t.index('static void worker(void *arg){')
t=t[:a]+t[b:]
(W/R).write_text(t)
print('patched')
