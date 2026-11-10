/* C3.11 ONLY. All handles stay in the 0x402dd... family.
 * Exclusive 0x101 proof: FS-EXCLUSIVE.md. Runtime collision test still required.
 * Callback convention is zero=success, including the normalized seek below. */
#include "native_files.h"
#include "../capture/live_alias.h"
/* Native file calls only accept the original mapping: map a cacheable alias back (same PA). */
static void *la_native_va(const void *p){uintptr_t a=(uintptr_t)p;
 return (void *)(a>=UINT64_C(0xc00000000)&&a<UINT64_C(0x1000000000)?a-UINT64_C(0x400000000):a);}
#define OPEN ((int32_t(*)(const char*,uint32_t,uint32_t))(uintptr_t)0x402dd4f8)
#define READ ((uint32_t(*)(int32_t,void*,uint32_t,uint32_t*))(uintptr_t)0x402dd6c8)
#define WRITE ((uint32_t(*)(int32_t,const void*,uint32_t,uint32_t*,void*))(uintptr_t)0x402dd78c)
#define CLOSE ((uint32_t(*)(int32_t))(uintptr_t)0x402dd558)
#define SEEK ((uint32_t(*)(int32_t,int64_t,uint32_t,uint64_t*))(uintptr_t)0x402dd8e8)
#define REMOVE ((uint32_t(*)(const char*))(uintptr_t)0x402dda4c)
static int result(live_native_file_context*c,uint32_t r){if(r)c->last_native_error=r;return r?-1:0;}
static int open_common(void*v,const char*p,int*out,uint32_t flags){
 live_native_file_context*c=v;int32_t fd=OPEN(p,flags,0x20);
 if(fd<0||(uint32_t)fd==0x7fff0000u){c->last_native_error=(uint32_t)fd;return -1;}
 *out=fd;return 0;
}
static int open_read(void*v,const char*p,int*out){return open_common(v,p,out,0);}
static int create(void*v,const char*p,int*out){return open_common(v,p,out,0x101);}
static int seek(void*v,int fd,uint64_t at,uint64_t*actual){
 live_native_file_context*c=v;if(at>INT64_MAX){c->last_native_error=0xffffffffu;return -1;}
 /* Native seek returns Boolean nonzero on success; transaction also checks *actual. */
 uint32_t r=SEEK(fd,(int64_t)at,0,actual);if(!r){c->last_native_error=0xffffffffu;return -1;}return 0;
}
static int read(void*v,int fd,void*p,size_t n,size_t*actual){
 live_native_file_context*c=v;uint32_t got=0;*actual=0;if(n>UINT32_MAX){c->last_native_error=0xffffffffu;return -1;}
 la_dc_civac((uintptr_t)p,n);uint32_t r=READ(fd,la_native_va(p),(uint32_t)n,&got);la_dc_ivac((uintptr_t)p,n);*actual=got;return result(c,r);
}
static int write(void*v,int fd,const void*p,size_t n,size_t*actual){
 live_native_file_context*c=v;uint32_t got=0;*actual=0;if(n>UINT32_MAX){c->last_native_error=0xffffffffu;return -1;}
 la_dc_cvac((uintptr_t)p,n);uint32_t r=WRITE(fd,la_native_va(p),(uint32_t)n,&got,0);*actual=got;return result(c,r);
}
static int close(void*v,int fd){return result(v,CLOSE(fd));}
static int remove_owned(void*v,const char*p){return result(v,REMOVE(p));}
static int cancelled(void*v){live_native_file_context*c=v;return c->cancelled?c->cancelled(c->user):0;}
static void yield(void*v){live_native_file_context*c=v;if(c->yield)c->yield(c->user);}
static int ensure_dir(void*v,const char*p){
 live_native_file_context*c=v;int32_t fd=-1;
 for(unsigned i=0;i<128;i++){c->dir_path[i]=p[i];if(!p[i])break;}c->dir_stage=1;
 uint32_t (*open_dir)(const char*,int32_t*)=(void*)(uintptr_t)0x402ddc6c;
 uint32_t (*close_dir)(int32_t)=(void*)(uintptr_t)0x402ddd6c;
 uint32_t (*mkdir_)(const char*)=(void*)(uintptr_t)0x402ddbdc;
 if(!open_dir(p,&fd))return result(c,close_dir(fd));
 c->dir_stage=2;uint32_t r=mkdir_(p);if(!r){c->dir_stage=3;return 0;}
 if(!open_dir(p,&fd))return result(c,close_dir(fd));
 c->dir_stage=4;return result(c,r);
}
static void encode_event(void*v,unsigned event){live_native_file_context*c=v;if(c->encode_event)c->encode_event(c->user,event);}
void live_native_file_ops(live_file_ops*out,live_native_file_context*c){
 c->last_native_error=0;out->opaque=c;out->open_read=open_read;out->create_exclusive=create;
 out->seek=seek;out->read=read;out->write=write;out->close=close;out->remove_owned=remove_owned;
 out->cancelled=cancelled;out->yield=yield;out->encode_event=encode_event;out->ensure_dir=ensure_dir;
}
