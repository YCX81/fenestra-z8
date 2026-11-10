/* C3.11 emulation layer. No writable code/RX state; all state is Nikon heap.
 * Native GUI callbacks only see cached scenes. Card reads happen here on CPU2. */
#include "runtime.h"
#include "ui.h"
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
void pb_set_key(uint32_t key){PbRuntime*r=pb_runtime_get();if(r&&(key==0||key==9)){STORE(&r->key,key);atomic_fetch_add_explicit(&r->key_epoch,1,memory_order_relaxed);atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_release);}}
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
 atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_acq_rel);
 /* Worker never invokes GUI APIs. An in-flight UI present sees gate=0 and
  * unlinks before acknowledging; otherwise queue removal on the owner. */
 uint32_t request=LOAD(&r->render_request);
 if(request!=3)STORE(&r->render_request,r->display.display.attached?2:0);
 if(r->preview_fd>=0){FN(0x402dd558,uint32_t(*)(int32_t))(r->preview_fd);r->preview_fd=-1;}
 if(r->playing)r->restores++;
 r->playing=0;
}
static int ready_to_write(PbRuntime*r){
 uint32_t idle=0;
 if(LOAD(&r->ui_leaving)||!atomic_compare_exchange_strong_explicit(&r->render_request,&idle,4,memory_order_acq_rel,memory_order_relaxed))return 0;
 atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_acq_rel);
 if(LOAD(&r->display.display.readers)){STORE(&r->render_request,0);return 0;}return 1;
}
static int present(PbRuntime*r){
 uint32_t id=0;
 if(LOAD(&r->ui_leaving)||!current(&id)||id!=r->current_id){STORE(&r->render_request,0);return 0;}
 atomic_fetch_add_explicit(&r->gate,3,memory_order_release); /* new enabled odd epoch */
 STORE(&r->render_request,1);return 1;
}
static int playback_owner(void){
 FxNativeView view=pb_native_current_view();
 return view.address==UINT64_C(0xa0284a0e0)&&view.id==2&&view.state==1;
}
uint32_t pb_ui_poll_wrap(void *flags){
 uint32_t native_result=pb_ui_old_poll(flags);PbRuntime*r=pb_runtime_get();
 if(!r)return native_result;
 r->ui_polls++;
 uint32_t request=LOAD(&r->render_request);
 /* MCU cancellation precedes native key dispatch, whereas the worker may
  * not yet have run. Do not wait for a queued hide to unlink a closed gate. */
 if(request==0&&r->display.display.attached&&!(LOAD(&r->gate)&1)){
  uint32_t idle=0;
  if(atomic_compare_exchange_strong_explicit(&r->render_request,&idle,2,memory_order_acq_rel,memory_order_relaxed))request=2;
 }
 if((request!=1&&request!=2)||!atomic_compare_exchange_strong_explicit(&r->render_request,&request,3,memory_order_acq_rel,memory_order_relaxed))return native_result;
 if(!playback_owner()){
  r->after_leave_requests++;atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_release);
  STORE(&r->render_request,0);return native_result;
 }
 if(request==1&&(LOAD(&r->gate)&1)&&!LOAD(&r->ui_leaving)){
  FxResult result=FX_OK;
  if(!r->display.display.attached)result=pb_display_attach(&r->display,&r->scene,&r->gate);
  if(result==FX_OK)result=pb_display_present(&r->display,&r->scene);
  if(result==FX_OK){r->ui_presented++;if(r->scene.data&&(*(uint32_t*)r->scene.data==320||*(uint32_t*)r->scene.data==1024))r->rendered_frames++;}
  else {r->ui_render_error=(uint32_t)result;atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_release);}
 }
 if(request==2||!(LOAD(&r->gate)&1)||LOAD(&r->ui_leaving)){
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
 STORE(&r->ui_leaving,1);atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_acq_rel);
 if(playback_owner()&&r->display.display.attached){pb_display_detach(&r->display);r->ui_detached++;}
 STORE(&r->render_request,0);
}
void pb_ui_state_notify(void){
 /* Native view2/state1 owner, before its business callback handles a key.
  * Normal Play exits without calling the separate event1 teardown handler. */
 PbRuntime*r=pb_runtime_get();if(!r||!playback_owner())return;
 r->ui_state_calls++;
 if(r->display.display.attached&&(!(LOAD(&r->gate)&1)||LOAD(&r->ui_leaving))){
  pb_display_detach(&r->display);r->ui_detached++;r->ui_state_detached++;
  if(LOAD(&r->render_request)!=4)STORE(&r->render_request,0);
 }
}
static void lookup(PbRuntime*r,uint32_t id){
 r->cache_valid=0;r->cached_id=id;r->lookup_count++;r->native_path[0]=0;r->directory[0]=0;
 if(FN(0x407b95e4,uint32_t(*)(uint32_t,char*))(id,r->native_path)||live_sync_directory(r->directory,r->native_path)){r->lookup_error=1;return;}
 uint8_t text[256];char p[64];uint32_t n=0;uint64_t size=0;
 if(live_sync_file(p,r->native_path,LIVE_FILE_MAN)){r->lookup_error=1;return;}
 if(readfile(p,text,sizeof(text),&n,0)||live_manifest_parse(text,n,&r->manifest)||
    memcmp(r->manifest.native_path,r->native_path,30)||r->manifest.volume_generation!=LOAD(&r->media_epoch)||
    readfile(r->native_path,0,0,0,&size)||size!=r->manifest.native_size){r->lookup_error=2;return;}
 r->photo_size=size;r->lookup_error=0;r->cache_valid=1;
}
static int start(PbRuntime*r,uint64_t stamp){
 char p[64];if(!r->i420||!r->pixels||live_sync_file(p,r->native_path,LIVE_FILE_LVP))return 0;
 int32_t fd=FN(0x402dd4f8,int32_t(*)(const char*,uint32_t,uint32_t))(p,0,0x20);
 if(fd<0||(uint32_t)fd==0x7fff0000)return 0;
 r->preview_fd=fd;uint64_t n=0,pos=0;uint8_t h[32];uint32_t got=0;
 if(!FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,0,2,&n)||
    !FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(fd,0,0,&pos)||pos||
    FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(fd,h,32,&got)||got!=32||
    lp_preview_open(h,n,&r->preview,LOAD(&r->key_epoch))||
    !((r->preview.width==320&&r->preview.height==212)||(r->preview.width==1024&&r->preview.height==682))){hide(r);return 0;}
 r->preview_size=n;r->start_us=stamp;r->last_frame=UINT32_MAX;r->playing=1;return 1;
}
static void frame(PbRuntime*r,uint64_t stamp){
 uint64_t off=0;int result=lp_preview_frame(&r->preview,stamp-r->start_us,LOAD(&r->key_epoch),&off);
 if(result){hide(r);if(result==1)r->blocked_key_epoch=0;return;}
 uint32_t index=(uint32_t)((off-32)/r->preview.frame_bytes);if(index==r->last_frame)return;
 uint64_t pos=0;uint32_t got=0;
 if(!FN(0x402dd8e8,uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(r->preview_fd,(int64_t)off,0,&pos)||pos!=off||
    FN(0x402dd6c8,uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(r->preview_fd,r->i420,r->preview.frame_bytes,&got)||got!=r->preview.frame_bytes){hide(r);return;}
 if(!ready_to_write(r))return;
 if(!r->pixels||pb_scene_frame(&r->scene,r->commands,r->pixels,PB_SCENE_DATA_BYTES,r->i420,r->preview.frame_bytes)!=FX_OK){hide(r);return;}
 uint32_t id=0;if(!current(&id)||id!=r->current_id||LOAD(&r->key_epoch)!=r->preview.epoch){hide(r);return;}
 if(present(r)){r->frames++;r->last_frame=index;}
}
static void worker(void *arg){
 PbRuntime*r=arg;r->started=1;uint32_t lastkey=LOAD(&r->key_epoch),lastid=0;
 for(;;){
  r->polls++;pb_settings_poll(r);
  if(!r->gc_attempted){
   r->gc_attempted=1;
   if(lock(r)){r->gc_result=live_native_sync_gc_boot(&r->journal,&r->sync);unlock(r);}
   else r->gc_result=-1;
  }
  uint32_t id=0,key=LOAD(&r->key_epoch);int active=current(&id)&&!LOAD(&r->shutdown);
  if(key!=lastkey){hide(r);r->blocked_key_epoch=key;lastkey=key;}
  if(!active){hide(r);r->current_id=0;STORE(&r->start_request,0);STORE(&r->ui_leaving,0);lastid=0;}
  else {
   if(id!=lastid){hide(r);r->current_id=id;lastid=id;lookup(r,id);r->blocked_key_epoch=0;}
   if(LOAD(&r->start_request)){
    STORE(&r->start_request,0);
    if(r->cache_valid&&LOAD(&r->key)==9)start(r,now_us());
   }
   if(r->playing)frame(r,now_us());
   else if(r->cache_valid&&!r->blocked_key_epoch&&!r->display.display.attached&&ready_to_write(r)){
    if(r->pixels&&pb_scene_badge_bitmap(&r->scene,r->commands,r->pixels,PB_SCENE_DATA_BYTES)==FX_OK)present(r);
   }
  }
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
 live_native_sync_init(&r->sync,'A',1);
 uintptr_t z=0;if(!__atomic_compare_exchange_n(p,&z,(uintptr_t)r,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)){FN(0x40d2c448,void(*)(void*))(r);return;}
 r->stack=FN(0x40d2c408,void*(*)(uint32_t,uint32_t))(65552,0);if(!r->stack){r->error=0xe001;return;}
 struct {uint32_t attributes,reserved;uintptr_t context,entry;uint32_t priority,reserved2;uint64_t stack_bytes;uintptr_t stack;uint32_t processor,affinity;} task={0,0,(uintptr_t)r,(uintptr_t)worker,30,0,65536,((uintptr_t)r->stack+15)&~(uintptr_t)15,3,4};
 int32_t tid=FN(0x40185064,int32_t(*)(const void*))(&task);
 if(tid<1||FN(0x401855e0,int32_t(*)(int32_t))(tid))r->error=0xe002;
}
void pb_system_notify(const void *message){
 PbRuntime*r=pb_runtime_get();if(!r||!message)return;
 uint32_t event=*(const uint32_t*)((const uint8_t*)message+32);
 if(event==7||event==24){
  atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_release);
  atomic_fetch_add_explicit(&r->key_epoch,1,memory_order_acq_rel);
  r->cache_valid=0;
  if(event==7)STORE(&r->shutdown,1);
  else {atomic_fetch_add_explicit(&r->media_epoch,1,memory_order_acq_rel);r->sync.poisoned=1;}
 }
}
uint32_t pb_key_notify(const uint8_t *body){
 pb_boot();PbRuntime*r=pb_runtime_get();if(!r||!body)return 0;
 static const uint8_t offsets[]={0x7,0x8,0x9,0xb,0xc,0xe,0xf,0x10,0x11,0x14,0x15,0x16,0x18,0x19,0x1b,0x1c,0x1e,0x1f,0x20,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x36,0x39,0x3b,0x3c,0x3d,0x3e,0x41,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x50,0x51,0x52,0x53,0x54,0x55,0x58,0x5c,0x5d,0x5e,0x5f,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,0x71,0x72,0x73,0x74,0x76,0x7a,0x7b};
 unsigned rising=0,rec=0,leaving=0;
 for(unsigned i=0;i<sizeof(offsets);i++){unsigned o=offsets[i];if(body[o]&&!r->previous_keys[o]){rising++;if(o==9)rec=1;if(o==7||o==0x43||o==0x44)leaving=1;}r->previous_keys[o]=body[o];}
 uint32_t id=0;
 int owns_rec=body[9]&&LOAD(&r->key)==9&&r->cache_valid&&
  (r->playing||!r->blocked_key_epoch)&&current(&id)&&id==r->current_id;
 if(!rising)return (uint32_t)owns_rec;
 if(leaving&&playback_owner())STORE(&r->ui_leaving,1);
 r->key_presses++;atomic_fetch_add_explicit(&r->key_epoch,1,memory_order_acq_rel);atomic_fetch_and_explicit(&r->gate,~UINT64_C(1),memory_order_release);
 if(rec&&owns_rec&&!r->playing)STORE(&r->start_request,1);
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
 PbRuntime*r=pb_runtime_get();LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id;
 if(valid)m=r->manifest;
 uint32_t status=pb_delete_original(id,flags);
 if(status==0x04320080&&valid){r->delete_success++;enqueue(r,LIVE_SYNC_DELETE,id,&m);}return status;
}
uint32_t pb_protect_wrap(void *object,uint32_t protect){
 PbRuntime*r=pb_runtime_get();uint32_t id=object?*(uint32_t*)((uint8_t*)object+0x28):0;
 LiveManifest m={0};int valid=r&&r->cache_valid&&r->cached_id==id;if(valid)m=r->manifest;
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
