/* C3.11 emulation layer. No writable code/RX state; all state is Nikon heap.
 * Native GUI callbacks only see cached scenes. Card reads happen here on CPU2. */
#include "runtime.h"
#include "ui.h"
#include "icon32_layout.h"
#include "fenestra_memory.h"
#include "fx_registry.h"
#define FN(a,t) ((t)(uintptr_t)(a))
#define LOAD(p) atomic_load_explicit((p),memory_order_acquire)
#define STORE(p,v) atomic_store_explicit((p),(v),memory_order_release)
extern uint32_t pb_delete_original(uint32_t,uint32_t);
extern uint32_t pb_protect_original(void *,uint32_t);
extern void pb_settings_poll(PbRuntime *);
extern uint32_t pb_ui_old_poll(void *);
extern uint32_t pb_ui_old_wait(void);
extern uint64_t pb_key_original(const uint8_t *,uintptr_t,uintptr_t);
static int ram(uintptr_t p,size_t n){return !(p&7)&&p>=UINT64_C(0xa00000000)&&p<=UINT64_C(0xa40000000)-n;}
/* Our runtime pointer lives in the shared registry hung off the core root's
 * spare slot (see fx_registry.h), next to plan A's Live Photo state. */
static uintptr_t *slot_create(int create){
 uintptr_t root=((volatile uintptr_t *)(uintptr_t)UINT64_C(0xa032b50c8))[2];
 if(!ram(root,664*96))return 0;
 uintptr_t s=*(uintptr_t *)(root+663*96+80);
 if(!ram(s,2536)||*(uint32_t *)(s+64)!=0x465a394d||!*(uint32_t *)(s+68))return 0;
 FxRegistry *g=fx_registry((uintptr_t *)(s+FX_REGISTRY_ROOT_OFFSET),create);
 return g?&g->playback:0;
}
static uintptr_t *slot(void){return slot_create(0);}
FxRegistry *pb_registry(void){
 uintptr_t root=((volatile uintptr_t *)(uintptr_t)UINT64_C(0xa032b50c8))[2];
 if(!ram(root,664*96))return 0;
 uintptr_t s=*(uintptr_t *)(root+663*96+80);
 if(!ram(s,2536)||*(uint32_t *)(s+64)!=0x465a394d||!*(uint32_t *)(s+68))return 0;
 return fx_registry((uintptr_t *)(s+FX_REGISTRY_ROOT_OFFSET),0);
}
PbRuntime *pb_runtime_get(void){uintptr_t *p=slot();if(!p)return 0;uintptr_t s=__atomic_load_n(p,__ATOMIC_ACQUIRE);if(!ram(s,sizeof(PbRuntime)))return 0;PbRuntime*r=(void*)s;return r->magic==PB_MAGIC&&r->bytes==sizeof(*r)?r:0;}
uint32_t pb_get_key(void){PbRuntime*r=pb_runtime_get();return r?LOAD(&r->key):9;}
/* Every cancellation changes the generation, even when the producer already
 * closed the gate. Only the holder of that generation can publish a scene. */
static uint64_t pb_close_gate(PbRuntime *r,uint64_t *before) {
    uint64_t old=LOAD(&r->gate),next;
    do {next=(old&~UINT64_C(1))+2u;}
    while(!atomic_compare_exchange_weak_explicit(&r->gate,&old,next,memory_order_seq_cst,memory_order_relaxed));
    if(before)*before=old;
    return next;
}
static void pb_clear_pending(PbRuntime *r) {
    uint32_t pending=LOAD(&r->render_request);
    while((pending==1u||pending==2u)&&!atomic_compare_exchange_weak_explicit(
          &r->render_request,&pending,0u,memory_order_acq_rel,memory_order_relaxed)){}
}
void pb_set_key(uint32_t key){PbRuntime*r=pb_runtime_get();if(r&&(key==0||key==9)){STORE(&r->key,key);atomic_fetch_add_explicit(&r->key_epoch,1,memory_order_relaxed);pb_close_gate(r,0);}}
static uint64_t now_us(void){uint64_t n,f;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(n),"=r"(f));return f?n/f*1000000+(n%f)*1000000/f:0;}
static void yield(void){FN(0x40187b00,int32_t(*)(uint32_t))(5);}
static int lock(PbRuntime*r){uint32_t z=0;return atomic_compare_exchange_strong_explicit(&r->journal_lock,&z,1,memory_order_acquire,memory_order_relaxed);}
static void unlock(PbRuntime*r){STORE(&r->journal_lock,0);}
static int readfile(const char *path,void *out,uint32_t cap,uint32_t *got,uint64_t *size){
 int32_t fd=FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))(path,0,0x20);
 if(fd<0||(uint32_t)fd==0x7fff0000)return -1;
 uint64_t n=0,pos=0;uint32_t count=0;
 int good=FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,0,2,&n)!=0;
 if(good&&out)good=n<=cap&&FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,0,0,&pos)&&!pos&&!FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(fd,out,(uint32_t)n,&count)&&count==n;
 uint32_t c=FN(0x402dd558,uint32_t(*)(int32_t))(fd);
 if(got)*got=count;if(size)*size=n;return good&&!c?0:-1;
}
static int current(uint32_t *id){
 void*v=*(void *volatile *)(uintptr_t)UINT64_C(0xa032280c8);
 if((uintptr_t)v!=UINT64_C(0xa0284a0e0)||FN(0x40393f6c,uint32_t(*)(void*))(v)!=2||FN(0x40393f28,uint32_t(*)(void*))(v)!=1)return 0;
 /* Native 4061b310 maps setting491a=1..3 to grid pages567..569. */
 if(FN(0x403ec398,uint32_t(*)(uint32_t))(0x491a)!=0)return 0;
 uint32_t *p=FN(0x407b4e78,uint32_t*(*)(void))();
 if(!ram((uintptr_t)p,4)||!*p)return 0;*id=*p;return 1;
}
static void hide(PbRuntime*r){
 if(r->audio_stage)r->audio_stop=1;   /* the worker's audio_step winds the service down */
 pb_close_gate(r,0);
 /* Worker never invokes GUI APIs. An in-flight UI present sees gate=0 and
  * unlinks before acknowledging; otherwise queue removal on the owner. */
 uint32_t request=LOAD(&r->render_request);
 while(request!=3u&&!atomic_compare_exchange_weak_explicit(&r->render_request,&request,
  r->display.display.attached?2u:0u,memory_order_acq_rel,memory_order_relaxed)){}
 if(r->preview_fd>=0){FN(0x402dd558,uint32_t(*)(int32_t))(r->preview_fd);r->preview_fd=-1;}
 if(r->playing)r->restores++;
 r->playing=0;
}
static int ready_to_write(PbRuntime *r,uint32_t key) {
    uint32_t idle=0;
    if(LOAD(&r->ui_leaving)||!atomic_compare_exchange_strong_explicit(
       &r->render_request,&idle,4u,memory_order_acq_rel,memory_order_relaxed))return 0;
    uint64_t before;
    r->compose_epoch=pb_close_gate(r,&before);
    if(LOAD(&r->shutdown)||LOAD(&r->ui_leaving)||key!=LOAD(&r->key_epoch)) {
        STORE(&r->render_request,0);return 0;
    }
    if(atomic_load_explicit(&r->display.display.readers,memory_order_seq_cst)) {
        /* No bytes changed. Restore the old frame only if no cancellation
         * intervened; publish it under a new epoch, never by reusing a ticket. */
        uint64_t expected=r->compose_epoch;
        if(before&1u)atomic_compare_exchange_strong_explicit(&r->gate,&expected,
                     expected|1u,memory_order_seq_cst,memory_order_relaxed);
        STORE(&r->render_request,0);return 0;
    }
    return 1;
}
static int present(PbRuntime *r,uint32_t key) {
    uint32_t id=0;
    uint64_t expected=r->compose_epoch;
    if(LOAD(&r->ui_leaving)||LOAD(&r->shutdown)||key!=LOAD(&r->key_epoch)||
       !current(&id)||id!=r->current_id||!atomic_compare_exchange_strong_explicit(
       &r->gate,&expected,expected|1u,memory_order_seq_cst,memory_order_relaxed)) {
        STORE(&r->render_request,0);return 0;
    }
    STORE(&r->render_request,1);return 1;
}
static int playback_owner(void){
 FxNativeView view=pb_native_current_view();
 return view.address==UINT64_C(0xa0284a0e0)&&view.id==2&&view.state==1;
}
/* Pair publication and UI bridge setup never wait on each other. */
static int pair_lock(PbRuntime*r){uint32_t z=0;return atomic_compare_exchange_strong_explicit(&r->pair_lock,&z,1,memory_order_acquire,memory_order_relaxed);}
/* ---- one-MOV bridge ---- */
__attribute__((noinline)) void pb_nb_observe(const void*state,uint32_t where){__asm__ volatile(""::"r"(state),"r"(where):"memory");}
static int nb_match(PbRuntime*r,const char*path){return r&&LOAD(&r->nb_stage)>=2u&&r->nb_epoch==LOAD(&r->media_epoch)&&path&&!memcmp(path,r->nb_from,30);}
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
 if(!o||!r||LOAD(&r->nb_stage)<2u||r->nb_epoch!=LOAD(&r->media_epoch))return o;
 uint8_t*c=r->nb_info;memcpy(c,o,sizeof(r->nb_info));
 c[4]=3u;c[5]|=4u;c[0xb]=(uint8_t)((c[0xb]&~3u)|1u);
 c[0xc]=(uint8_t)r->nb_w;c[0xd]=(uint8_t)(r->nb_w>>8);c[0xe]=(uint8_t)r->nb_h;c[0xf]=(uint8_t)(r->nb_h>>8);
 return c;
}
/* The photo's node in the stock DCF index: the lookup 4085fa38 falls back to
 * (directory 4085e91c, then node 4086c378). Member formats are the u16 at +0x24.
 * Called with the DCF lock held; takes the slot lock the stock readback takes. */
static uint16_t *nb_mask(PbRuntime*r,uint32_t id,int set){
 uint32_t card=id>>29&3u;if(card>1u)return 0;
 uintptr_t slot=((volatile uintptr_t*)(uintptr_t)UINT64_C(0xa04822af8))[card];
 if(slot<UINT64_C(0x800000000)||slot>=UINT64_C(0x1000000000)||(slot&7u))return 0;   /* e149e6dc0 on C3.11 */
 FN(0x40875834,void(*)(uintptr_t,uint32_t,uint32_t))(slot+0x222138u,0x64,0x64);
 uintptr_t dir=FN(0x4085e91c,uintptr_t(*)(uintptr_t,uint32_t))(slot,id>>19&0x3ffu);
 uintptr_t node=dir?FN(0x4086c378,uintptr_t(*)(uintptr_t,uint32_t,void*))(dir,id,0):0;
 uint16_t*m=node>=slot&&node<=slot+0x222188u-0x26u?(uint16_t*)(node+0x24u):0;
 /* Never borrow a real MOV member; clear only the node we actually changed. */
 if(m){if(set){if(*m&0x20u)m=0;else *m|=0x20u;}
  else if((uintptr_t)m==r->nb_node&&r->nb_card_epoch==LOAD(&r->card_epoch[card]))*m&=(uint16_t)~0x20u;}
 FN(0x408758b4,void(*)(uintptr_t))(slot+0x222138u);
 return m;
}
static void nb_end(PbRuntime*r){
 if(LOAD(&r->nb_stage)<2u)return;
 FN(0x4085d348,void(*)(void))();
 if(LOAD(&r->nb_stage)>=2u){if(r->nb_bit)nb_mask(r,r->nb_photo,0);r->nb_bit=0;STORE(&r->nb_stage,0);}
 FN(0x4085d364,void(*)(void))();
 pb_nb_observe(&r->nb_request,3);
}
uint32_t pb_native_return(uint32_t kind,uint32_t page){
 PbRuntime*r=pb_runtime_get();
 if(r&&kind==3&&page==0x552&&LOAD(&r->nb_stage)==2u){
  r->nb_idle_us=now_us();STORE(&r->nb_stage,3);r->nb_returns++;pb_nb_observe(&r->nb_request,4);
  uint32_t selected=FN(0x403ec398,uint32_t(*)(uint32_t))(0x4910);
  if(r->nb_card_epoch==LOAD(&r->card_epoch[r->nb_photo>>29&1u])&&(selected&~15u)==(r->nb_photo&~15u))
   FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_photo);
 }
 return FN(0x403ad6a4,uint32_t(*)(uint32_t,uint32_t))(kind,page);
}
static void nb_start(PbRuntime*r){
 uint32_t id=0;char to[64];
 if(LOAD(&r->nb_stage)||!pair_lock(r))return;
 if(!playback_owner()||!current(&id)||(id>>29&3u)>1u||id!=r->current_id||id!=r->cached_id||!r->cache_valid||!r->nb_w||LOAD(&r->shutdown)||
    r->lookup_epoch!=LOAD(&r->media_epoch)||live_sync_file(to,r->manifest.native_path,LIVE_FILE_MOV)){STORE(&r->pair_lock,0);r->nb_fail=1;r->nb_cur=id;pb_nb_observe(&r->nb_request,1);return;}
 memcpy(r->nb_from,r->native_path,64);memcpy(r->nb_from+25,".MOV",5);memcpy(r->nb_to,to,64);
 FN(0x4085d348,void(*)(void))();
 r->nb_epoch=r->lookup_epoch;r->nb_card_epoch=LOAD(&r->card_epoch[id>>29&1u]);
 uint16_t*m=r->nb_epoch==LOAD(&r->media_epoch)?nb_mask(r,id,1):0;r->nb_node=(uintptr_t)m;r->nb_bit=m!=0;
 r->nb_photo=id;r->nb_movie=(id&~15u)|5u;r->nb_idle_us=0;
 STORE(&r->nb_stage,m?2u:0u);STORE(&r->pair_lock,0);
 FN(0x4085d364,void(*)(void))();
 if(!m){r->nb_fail=2;pb_nb_observe(&r->nb_request,1);return;}
 r->nb_starts++;r->nb_fail=0;
 pb_ui_leave();
 FN(0x403ec3a0,void(*)(uint32_t,uint32_t))(0x4910,r->nb_movie);
 pb_nb_observe(&r->nb_request,2);
 FN(0x406364d0,void(*)(void))();
 FN(0x403fb660,void(*)(uint32_t))(1);
 FN(0x403ad6a4,void(*)(uint32_t,uint32_t))(3,0x56c);
 FN(0x403fb660,void(*)(uint32_t))(0);
}
uint32_t pb_ui_poll_wrap(void *flags){
 uint32_t native_result=pb_ui_old_poll(flags);PbRuntime*r=pb_runtime_get();
 if(!r)return native_result;
 r->ui_polls++;
 if(atomic_exchange_explicit(&r->nb_request,0,memory_order_acq_rel)){nb_start(r);if(LOAD(&r->nb_stage)==2u)return native_result;}
 uint32_t request=LOAD(&r->render_request);
 uint32_t pose=0u;
 if(r->display.display.attached||request==1)pose=icon32_orientation();
 /* MCU cancellation precedes native key dispatch, whereas the worker may
  * not yet have run. Do not wait for a queued hide to unlink a closed gate. */
 if(request==0&&r->display.display.attached&&playback_owner()) {
  uint32_t idle=0;
  if(!atomic_compare_exchange_strong_explicit(&r->render_request,&idle,3u,memory_order_acq_rel,memory_order_relaxed))return native_result;
  if(!(LOAD(&r->gate)&1)||!pb_display_is_current(&r->display))request=2;
  else if(r->display.display.scene==&r->badge_scene&&(pose!=r->badge_pose||LOAD(&r->badge_blocked)))request=1;
  /* v37: repaint the badge every 0.5 s; a native redraw of the page may have covered it. */
  else if(r->display.display.scene==&r->badge_scene&&now_us()-r->badge_present_us>500000u){r->badge_heals++;request=1;}
  else {STORE(&r->render_request,0);return native_result;}
 } else if((request!=1&&request!=2)||!atomic_compare_exchange_strong_explicit(&r->render_request,&request,3,memory_order_acq_rel,memory_order_relaxed))return native_result;
 if(!playback_owner()){
  r->after_leave_requests++;pb_close_gate(r,0);
  STORE(&r->render_request,0);return native_result;
 }
 if(request==1&&(LOAD(&r->gate)&1)&&!LOAD(&r->ui_leaving)){
  FxResult result=FX_OK;
  if(!r->display.display.attached)result=pb_display_attach(&r->display,&r->scene,&r->gate);
  if(result==FX_OK)result=pb_display_present(&r->display,&r->scene);
  r->badge_present_us=now_us();
  if(result==FX_OK){r->badge_pose=pose;r->ui_presented++;if(r->scene.data&&r->display.display.scene!=&r->badge_scene)r->rendered_frames++;}
  else {r->ui_render_error=(uint32_t)result;pb_close_gate(r,0);}
 }
 if(request==2||!(LOAD(&r->gate)&1)||LOAD(&r->ui_leaving)){
  pb_close_gate(r,0);
  pb_display_detach(&r->display);r->ui_detached++;
 }
 STORE(&r->render_request,0);return native_result;
}
uint32_t pb_ui_wait_wrap(void){
 uint32_t wait=pb_ui_old_wait();PbRuntime*r=pb_runtime_get();
 return r&&playback_owner()&&wait>16?16:wait;
}
void pb_ui_leave(void){
 /* Called on the native owner BEFORE playback teardown/state transfer. */
 PbRuntime*r=pb_runtime_get();if(!r)return;
 STORE(&r->ui_leaving,1);pb_close_gate(r,0);
 if(playback_owner()&&r->display.display.attached){pb_display_detach(&r->display);r->ui_detached++;}
 pb_clear_pending(r);
}
void pb_ui_state_notify(void){
 /* Native view2/state1 owner, before its business callback handles a key.
  * Normal Play exits without calling the separate event1 teardown handler. */
 PbRuntime*r=pb_runtime_get();if(!r||!playback_owner())return;
 r->ui_state_calls++;
 if(r->display.display.attached&&(LOAD(&r->ui_leaving)||(!(LOAD(&r->gate)&1)&&LOAD(&r->render_request)!=4u))){
  pb_display_detach(&r->display);r->ui_detached++;r->ui_state_detached++;
  pb_clear_pending(r);
 }
}
static uint32_t be32(const uint8_t*p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void mov_size(PbRuntime*r){
 char p[64];uint8_t h[16];uint64_t at=0,lim=0,pos=0;uint32_t got=0,w=0,hh=0,nikon=0;r->nb_w=r->nb_h=0;
 if(live_sync_file(p,r->manifest.native_path,LIVE_FILE_MOV))return;
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
/* The displayed photo remains native_path/current_id. The validated Live
 * owner is manifest.native_path and may be a JPEG on the other card. */
static int live_manifest_for(PbRuntime*r,const char*photo){
 uint8_t text[256];char p[64];uint32_t n=0;uint64_t size=0;
 if(live_sync_file(p,photo,LIVE_FILE_MAN)||readfile(p,text,sizeof(text),&n,0)||
    live_manifest_parse(text,n,&r->manifest)||memcmp(r->manifest.native_path,photo,30)||
    readfile(photo,0,0,0,&size)||size!=r->manifest.native_size)return 0;
 r->photo_size=size;return 1;
}
static int live_jpeg_for(PbRuntime*r,uint32_t id){
 char photo[64]={0};
 /* Unlike 407b95e4 (group primary), this ref retains precise format 0. */
 void *ref=FN(0x4085a8e0,void*(*)(uint32_t))(id&~15u);
 if(!ref)return 0;
 const char *path=FN(0x4085c0d0,const char*(*)(void*))(ref);
 if(path)memcpy(photo,path,30);
 FN(0x4085aa7c,void(*)(void*))(ref);
 return !memcmp(photo+25,".JPG",5)&&live_manifest_for(r,photo);
}
static void lookup(PbRuntime*r,uint32_t id){
 uint32_t epoch=LOAD(&r->media_epoch);
 r->lookup_us=now_us();r->cache_valid=0;r->lookup_epoch=epoch;r->cached_id=id;r->lookup_count++;
 r->nb_w=r->nb_h=0;r->native_path[0]=0;r->directory[0]=0;
 if(FN(0x407b95e4,uint32_t(*)(uint32_t,char*))(id,r->native_path)||
    live_sync_directory(r->directory,r->native_path)){r->lookup_error=1;return;}
 if(!live_manifest_for(r,r->native_path)){
  if(memcmp(r->native_path+25,".NEF",5)){r->lookup_error=2;return;}
  if(!live_jpeg_for(r,id)){
   /* Same query as Nikon's "Jump to copy on other card", not a card-bit
    * toggle or filename guess. No DCF/journal/GUI lock is held here. */
   uint64_t other=0;
   uint32_t queried=FN(0x403fb59c,uint32_t(*)(uint32_t,uint32_t,uint64_t*))(id,2u,&other);
   uint32_t peer=(uint32_t)other;
   if(!(queried&1u)||!peer||(other>>32&2u)||(peer>>29&3u)>1u||
      (peer>>29&3u)==(id>>29&3u)||!live_jpeg_for(r,peer)){
    r->lookup_error=2;return;
   }
  }
 }
 mov_size(r);
 if(!r->nb_w){r->lookup_error=3;return;}
 if(epoch!=LOAD(&r->media_epoch)||LOAD(&r->shutdown)){r->lookup_error=4;return;}
 r->lookup_epoch=epoch;r->lookup_error=0;r->cache_valid=1;
}

/* ---- preview sound (protocol and addresses: tools-v37/patch_audio.py) ---- */
#define PB_AUDIO_SERVICE (*(volatile uint32_t*)(uintptr_t)UINT64_C(0xa031f8d28))   /* 0 = playback service idle */
#define PB_AUDIO_DRIVER (*(volatile uint32_t*)(uintptr_t)UINT64_C(0xa02d4cb48))    /* 1 = audio driver idle */
#define PB_AUDIO_RING (*(volatile uint64_t*)(uintptr_t)UINT64_C(0xa031f8bb8))      /* stock fixed audio buffer */
#define PB_AUDIO_CAPACITY (*(volatile uint32_t*)(uintptr_t)UINT64_C(0xa031f8bc0))
/* Native 40d24148 allocates 1,000,000 B and publishes that capacity at +8.
 * Validate it before using 5 seconds: <=4 seconds of PCM + >=1 second of zero
 * tail while the worker completes stop. Never enlarge the native allocation. */
#define PB_AUDIO_BLOCK_BYTES 192000u
#define PB_AUDIO_BLOCKS 5u
#define PB_AUDIO_RING_BYTES (PB_AUDIO_BLOCK_BYTES*PB_AUDIO_BLOCKS)
#define PB_AUDIO_PLAY_BYTES 768000u
int pb_audio_handoff(void){
 PbRuntime*r=pb_runtime_get();if(!r)return 1;
 /* This word also claims the shared PCM before any copy. Observing idle here
  * therefore cannot race a worker that has begun copying but not posted14. */
 uint32_t old=LOAD(&r->audio_gate);
 for(;;){
  if(old&1u)return !(old&2u);
  if(atomic_compare_exchange_weak_explicit(&r->audio_gate,&old,(old+4u)|1u,memory_order_acq_rel,memory_order_acquire))return !(old&2u);
 }
}
static void worker(void *arg){
 PbRuntime*r=arg;r->started=1;uint32_t lastkey=LOAD(&r->key_epoch),lastid=0;
 for(;;){
  r->polls++;pb_settings_poll(r);
  /* card-slot: a card was mounted/removed. Drop queued follow-ups from the old
   * media session, start a new one and census again (both volumes). */
  if(LOAD(&r->sync_reset)&&lock(r)){
   r->journal=(LiveSyncJournal){0};r->sync.generation=LOAD(&r->media_epoch);
   r->sync.gc_ready=0;r->gc_attempted=0;r->sync_resets++;STORE(&r->sync_reset,0);unlock(r);
  }
  if(!r->gc_attempted&&!LOAD(&r->sync_reset)){
   r->gc_attempted=1;
   if(lock(r)){r->gc_result=live_native_sync_gc_boot(&r->journal,&r->sync);unlock(r);}
   else r->gc_result=-1;
  }
  uint32_t id=0,key=LOAD(&r->key_epoch);int active=current(&id)&&!LOAD(&r->shutdown);
  if(key!=lastkey){hide(r);r->blocked_key_epoch=key;lastkey=key;}
  /* v37: a key press hides the badge for the native redraw; it used to stay away until
   * another photo was shown. Bring it back once the keys have been quiet for 0.7 s. */
  else if(r->blocked_key_epoch&&!r->playing&&now_us()-r->last_key_us>700000u)r->blocked_key_epoch=0;
  if(!active){hide(r);r->current_id=0;STORE(&r->start_request,0);STORE(&r->ui_leaving,0);lastid=0;}
  else if(!LOAD(&r->nb_stage)){
   if((id!=lastid||r->lookup_epoch!=LOAD(&r->media_epoch)||(!r->cache_valid&&now_us()-r->lookup_us>=1000000u))&&pair_lock(r)){
    if(!LOAD(&r->nb_stage)){hide(r);r->cache_valid=0;r->current_id=id;lastid=id;lookup(r,id);r->blocked_key_epoch=0;}
    STORE(&r->pair_lock,0);}

   /* picture and sound start together: hold the first frame while the service opens (bounded) */
   if(r->cache_valid&&r->lookup_epoch==LOAD(&r->media_epoch)&&!LOAD(&r->nb_stage)&&!r->blocked_key_epoch&&!r->display.display.attached&&ready_to_write(r,key)){
    if(r->pixels&&pb_scene_badge_bitmap(&r->scene,r->commands,r->pixels,PB_SCENE_DATA_BYTES)==FX_OK)present(r,key);
    else if(pb_scene_badge(&r->scene,r->commands)==FX_OK)present(r,key);
    else STORE(&r->render_request,0);
   }
  }
  if(LOAD(&r->nb_stage)==2u){
   /* left playback altogether (mode change, power): restore after 1 s. Still in the playback view
    * but 4910 no longer names the clip for 5 s: the page was left by another route, close lazily. */
   int gone=!playback_owner()||LOAD(&r->shutdown)||r->nb_epoch!=LOAD(&r->media_epoch);
   if(!gone&&FN(0x403ec398,uint32_t(*)(uint32_t))(0x4910)==r->nb_movie)r->nb_idle_us=0;
   else if(!r->nb_idle_us)r->nb_idle_us=now_us();
   else if(now_us()-r->nb_idle_us>(gone?1000000u:5000000u)){nb_end(r);r->nb_cleanups++;if(playback_owner()){STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;}}
  }
  else if(LOAD(&r->nb_stage)==3u&&now_us()-r->nb_idle_us>2500000u){nb_end(r);STORE(&r->ui_leaving,0);r->blocked_key_epoch=0;}
  if(lock(r)){live_native_sync_step(&r->journal,&r->sync);unlock(r);}
  yield();
 }
}
void pb_boot(void){
 uintptr_t*p=slot_create(1);if(!p||__atomic_load_n(p,__ATOMIC_ACQUIRE))return;
 PbRuntime*r=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(sizeof(*r),0);if(!r)return;
 memset(r,0,sizeof(*r));r->magic=PB_MAGIC;r->bytes=sizeof(*r);r->key=9;r->media_epoch=1;r->preview_fd=-1;
 r->i420=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(PB_PREVIEW_FRAME_BYTES,0);
 r->pixels=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(PB_SCENE_DATA_BYTES,0);
 if(!r->i420||!r->pixels)r->error=0xe003;   /* no preview playback; the rest keeps working */
 r->jpeg=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(LP_JPEG_MAX+16u+(uint32_t)sizeof(LpJpeg),0);
 if(r->jpeg)r->decoder=(LpJpeg*)(void*)(r->jpeg+LP_JPEG_MAX+16u);   /* else only legacy LVP previews play */
 live_native_sync_init(&r->sync,'A',1);
 uintptr_t z=0;if(!__atomic_compare_exchange_n(p,&z,(uintptr_t)r,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)){FN(0x40d2c448,void(*)(void*))(r);return;}
 FxRegistry*g=pb_registry();if(g)fx_registry_claim(&g->spare[0],(uintptr_t)pb_audio_handoff);
 r->stack=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(65552,0);if(!r->stack){r->error=0xe001;return;}
 struct {uint32_t attributes,reserved;uintptr_t context,entry;uint32_t priority,reserved2;uint64_t stack_bytes;uintptr_t stack;uint32_t processor,affinity;} task={0,0,(uintptr_t)r,(uintptr_t)worker,30,0,65536,((uintptr_t)r->stack+15)&~(uintptr_t)15,3,4};
 int32_t tid=FN(0x40185064,int32_t(*)(const void*))(&task);
 if(tid<1||FN(0x401855e0,int32_t(*)(int32_t))(tid))r->error=0xe002;
}
void pb_system_notify(const void *message){
 PbRuntime*r=pb_runtime_get();if(!r||!message)return;
 uint32_t event=*(const uint32_t*)((const uint8_t*)message+32);
 /* Native card lifecycle events (including format) carry slot at +40. */
 if(event==7||(event>=22u&&event<=31u)){
  pb_close_gate(r,0);
  atomic_fetch_add_explicit(&r->key_epoch,1,memory_order_acq_rel);
  r->cache_valid=0;
  if(event==7)STORE(&r->shutdown,1);
  else {uint32_t card=*(const uint32_t*)((const uint8_t*)message+40);
   /* Invalidate old pairings before publishing a new source-card identity. */
   atomic_fetch_add_explicit(&r->media_epoch,1,memory_order_acq_rel);
   for(uint32_t i=0;i<2u;i++)if(card==i||card>1u)atomic_fetch_add_explicit(&r->card_epoch[i],1,memory_order_acq_rel);
   __atomic_store_n(&r->sync.generation,0,__ATOMIC_RELEASE);STORE(&r->sync_reset,1);}
 }
}
uint32_t pb_key_notify(const uint8_t *body){
 pb_boot();PbRuntime*r=pb_runtime_get();if(!r||!body)return 0;
 static const uint8_t offsets[]={0x7,0x8,0x9,0xb,0xc,0xe,0xf,0x10,0x11,0x14,0x15,0x16,0x18,0x19,0x1b,0x1c,0x1e,0x1f,0x20,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x36,0x39,0x3b,0x3c,0x3d,0x3e,0x41,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x50,0x51,0x52,0x53,0x54,0x55,0x58,0x5c,0x5d,0x5e,0x5f,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,0x71,0x72,0x73,0x74,0x76,0x7a,0x7b};
 unsigned rising=0,rec=0,leaving=0;
 for(unsigned i=0;i<sizeof(offsets);i++){unsigned o=offsets[i];if(body[o]&&!r->previous_keys[o]){rising++;if(o==9)rec=1;if(o==7||o==0x43||o==0x44)leaving=1;}r->previous_keys[o]=body[o];}
 uint32_t id=0;
 int owns_rec=body[9]&&LOAD(&r->key)==9&&r->cache_valid&&!LOAD(&r->nb_stage)&&!LOAD(&r->shutdown)&&r->lookup_epoch==LOAD(&r->media_epoch)&&
  (r->playing||!r->blocked_key_epoch)&&current(&id)&&id==r->current_id&&id==r->cached_id;
 if(!rising)return (uint32_t)owns_rec;
 r->last_key_us=now_us();
 if(leaving&&playback_owner())STORE(&r->ui_leaving,1);
 r->key_presses++;atomic_fetch_add_explicit(&r->key_epoch,1,memory_order_acq_rel);pb_close_gate(r,0);
 if(rec&&owns_rec)STORE(&r->nb_request,1);
 return (uint32_t)owns_rec;
}
uint64_t pb_key_decode_wrap(const uint8_t *body,uintptr_t arg1,uintptr_t arg2){
 if(!pb_key_notify(body))return pb_key_original(body,arg1,arg2);
 /* The external MCU packet remains immutable. Native dispatch receives all
  * other contacts intact; explicit Live key ownership consumes only REC. */
 uint8_t native_body[124];memcpy(native_body,body,sizeof(native_body));native_body[9]=0;
 PbRuntime*r=pb_runtime_get();if(r)r->consumed_rec++;
 return pb_key_original(native_body,arg1,arg2);
}
static void enqueue(PbRuntime*r,unsigned op,uint32_t id,const LiveManifest *m){
 if(!r||!m||!id)return;
 if(lock(r)){if(live_sync_enqueue(&r->journal,op,m))r->journal_drops++;unlock(r);}else r->journal_drops++;
}
uint32_t pb_delete_wrap(uint32_t id,uint32_t flags){
 PbRuntime*r=pb_runtime_get();LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id&&
  r->lookup_epoch==LOAD(&r->media_epoch)&&!memcmp(r->native_path,r->manifest.native_path,30);
 if(valid)m=r->manifest;
 if(r&&LOAD(&r->nb_stage)==2u&&id==r->nb_movie){r->nb_refused++;return 0x0432008bu;}
 if(r)nb_end(r);
 uint32_t status=pb_delete_original(id,flags);
 if(status==0x04320080&&valid){r->delete_success++;enqueue(r,LIVE_SYNC_DELETE,id,&m);}return status;
}
uint32_t pb_protect_wrap(void *object,uint32_t protect){
 PbRuntime*r=pb_runtime_get();uint32_t id=object?*(uint32_t*)((uint8_t*)object+0x28):0;
 LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id&&
  r->lookup_epoch==LOAD(&r->media_epoch)&&!memcmp(r->native_path,r->manifest.native_path,30);if(valid)m=r->manifest;
 uint32_t status=pb_protect_original(object,protect);
 if(status==0x04320080&&valid){r->protect_success++;enqueue(r,LIVE_SYNC_PROTECT,id,&m);}return status;
}
/* Versioned read-only diagnostic offsets consumed by the driver. */
const uint32_t pb_runtime_metadata[]={PB_MAGIC,1,sizeof(PbRuntime),2528,
 offsetof(PbRuntime,started),offsetof(PbRuntime,error),offsetof(PbRuntime,polls),
 offsetof(PbRuntime,current_id),offsetof(PbRuntime,cache_valid),offsetof(PbRuntime,lookup_count),
 offsetof(PbRuntime,lookup_error),offsetof(PbRuntime,frames),offsetof(PbRuntime,playing),
 offsetof(PbRuntime,delete_success),offsetof(PbRuntime,protect_success),offsetof(PbRuntime,journal_drops),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,display)+offsetof(FxNativeDisplay,attached),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,display)+offsetof(FxNativeDisplay,error),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,display)+offsetof(FxNativeDisplay,callbacks),
 offsetof(PbRuntime,native_path),offsetof(PbRuntime,manifest),offsetof(PbRuntime,journal),
 offsetof(PbRuntime,sync)+offsetof(LiveNativeSync,last_error),
 offsetof(PbRuntime,sync)+offsetof(LiveNativeSync,steps),
 offsetof(PbRuntime,sync)+offsetof(LiveNativeSync,gc_ready),offsetof(PbRuntime,gc_result),
 offsetof(PbRuntime,render_request),offsetof(PbRuntime,ui_leaving),offsetof(PbRuntime,ui_polls),
 offsetof(PbRuntime,ui_presented),offsetof(PbRuntime,ui_detached),offsetof(PbRuntime,ui_render_error),
 offsetof(PbRuntime,rendered_frames),offsetof(PbRuntime,consumed_rec),offsetof(PbRuntime,after_leave_requests),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,images_created),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,images_destroyed),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,paths_created),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,paths_destroyed),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,paints_created),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,paints_destroyed),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,unlinks),
 offsetof(PbRuntime,display)+offsetof(PbNativeDisplay,skipped_owner),
 offsetof(PbRuntime,ui_state_calls),offsetof(PbRuntime,ui_state_detached)};
