/* Prototype core.jobs implementation. Included by module_session.c only: it
 * needs the private session layout. Emulation prototype, not hardware qualified.
 *
 * Trust rules:
 * - EL0 pointers and shared-page bytes are untrusted. Paths are copied once and
 *   classified on the copy; IO addresses must lie entirely in a job buffer.
 * - All syscalls run on this job task on ORIGINAL tables, never inside the gate.
 * - A job fault or timeout never resumes EL0. The worker turns it into the normal
 *   module Panic; buffers are retained and never reused until reboot. */
#define FX_JOB_TEST 1 /* prototype: fault-injection syscall; remove outside emulation */
#include "fenestra_jobs.h"
#include "task.h"
#include <stdatomic.h>
#define JOB_CONTEXT 0u
#define JOB_L2 16384u
#define JOB_L2_TABLES 4u
#define JOB_NATIVE (JOB_L2+JOB_L2_TABLES*4096u)
#define JOB_NATIVE_BYTES 65536u
#define JOB_API (JOB_NATIVE+JOB_NATIVE_BYTES)
#define JOB_SHARED (JOB_API+4096u)
#define JOB_GUARD0 (JOB_SHARED+4096u)
#define JOB_USER (JOB_GUARD0+4096u)
#define JOB_USER_BYTES 65536u
#define JOB_GUARD1 (JOB_USER+JOB_USER_BYTES)
#define JOB_EXTRA (JOB_GUARD1+4096u)
#define FX_JOB_PRIORITY 28u
#define FX_YIELD ((FxResult)14)
#define BLOCK2M (2u*1024u*1024u)
#define ALLOCATE(n) FN(UINT64_C(0x40d2c408),void *(*)(uint32_t,uint32_t))((n),0u)
#define RELEASE(p) FN(UINT64_C(0x40d2c448),void (*)(void *))(p)
#define CARD_OPEN FN(UINT64_C(0x402dd4f8),int32_t (*)(const char *,uint32_t,uint32_t))
#define CARD_READ FN(UINT64_C(0x402dd6c8),uint32_t (*)(int32_t,void *,uint32_t,uint32_t *))
#define CARD_WRITE FN(UINT64_C(0x402dd78c),uint32_t (*)(int32_t,const void *,uint32_t,uint32_t *,void *))
#define CARD_CLOSE FN(UINT64_C(0x402dd558),uint32_t (*)(int32_t))
#define CARD_SEEK FN(UINT64_C(0x402dd8e8),uint32_t (*)(int32_t,int64_t,uint32_t,uint64_t *))
#define CARD_REMOVE FN(UINT64_C(0x402dda4c),uint32_t (*)(const char *))
#define CARD_RENAME FN(UINT64_C(0x402dd9fc),uint32_t (*)(const char *,const char *))
#define DIR_OPEN FN(UINT64_C(0x402ddc6c),uint32_t (*)(const char *,int32_t *))
#define DIR_NEXT FN(UINT64_C(0x402ddd1c),uint32_t (*)(int32_t,void *))
#define DIR_CLOSE FN(UINT64_C(0x402ddd6c),uint32_t (*)(int32_t))
typedef struct {void *raw;uintptr_t base;uint32_t bytes,used;} JobBuffer;
typedef struct {int32_t fd;uint32_t used,create,uncertain;uint64_t position,size;} JobFile;
typedef struct {
    FxZ8ExecContext exec; /* first: the gate receives this pointer */
    FxZ8ModuleSession *session;FxPlatformOwner *owner;
    FxZ8TaskScope task;
    _Atomic uint32_t state,cancel,error,started;
    FxJobStart start;
    uint64_t entry,stack_top,last_heartbeat,last_beat_ms,begin_ms,cancel_ms,result;
    uint64_t resumes,syscalls,sleeps,read_bytes,write_bytes,memory_bytes,memory_peak,native_error,elapsed_ms;
    int32_t module_result;uint32_t l2_used,photos,inject_after;
    FxJobApi *api;FxJobShared *shared;
    char module_id[16];
    JobBuffer buffers[FX_JOB_MAX_BUFFERS];
    JobFile files[FX_JOB_MAX_FILES];
    int32_t dir;uint32_t dir_open;
    char created[FX_JOB_MAX_FILES][48];
} Job;
_Static_assert(sizeof(Job)<=JOB_L2,"job context pages");
_Static_assert(offsetof(Job,exec)==0,"gate context prefix");
static uint64_t job_now(void) {
    uint64_t count,freq;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(count),"=r"(freq));
    return count/freq*1000u+count%freq*1000u/freq;
}
static int job_active(const Job *j) {
    uint32_t state=atomic_load_explicit(&j->state,memory_order_acquire);
    return state==FX_JOB_QUEUED || state==FX_JOB_RUNNING;
}
/* ---- path policy: the core's only filesystem authority for a job ---- */
enum {PATH_DENY,PATH_PRIVATE,PATH_PHOTO};
static int job_upper(char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';}
static int job_same(const char *a,const char *b,uint32_t n){for(uint32_t i=0;i<n;++i)if(a[i]!=b[i])return 0;return 1;}
static uint32_t job_length(const char *p,uint32_t limit){uint32_t n=0;while(n<limit&&p[n])++n;return n;}
static int job_ext(const char *e,uint32_t n,const char *wanted) {
    return job_length(wanted,8u)==n && job_same(e,wanted,n);
}
/* DCIM file: <A|B>:\DCIM\<1-8 upper>\<1-24 upper>.<ext>; returns the extension. */
static const char *job_photo(const char *p,uint32_t n,uint32_t *ext_bytes) {
    if(n<12u || (p[0]!='A' && p[0]!='B') || !job_same(p+1,":\\DCIM\\",7u))return 0;
    uint32_t at=8u,dir=0u;while(at<n && job_upper(p[at])){++at;++dir;}
    if(!dir || dir>8u || at>=n || p[at]!='\\')return 0;
    ++at;uint32_t stem=0u;while(at<n && job_upper(p[at])){++at;++stem;}
    if(!stem || stem>24u || at>=n || p[at]!='.')return 0;
    const char *ext=p+at+1u;uint32_t e=n-at-1u;
    if(!e || e>4u)return 0;
    for(uint32_t i=0;i<e;++i)if(!job_upper(ext[i]))return 0;
    *ext_bytes=e;return ext;
}
/* A:\FENESTRA\<module id>.<name>. Never a module, flag or startup record. */
static int job_private(const Job *j,const char *p,uint32_t n) {
    static const char prefix[]="A:\\FENESTRA\\";
    uint32_t id=job_length(j->module_id,16u),at=sizeof(prefix)-1u;
    if(!id || n<=at+id+1u || n>63u || !job_same(p,prefix,at) || !job_same(p+at,j->module_id,id) || p[at+id]!='.')return 0;
    at+=id+1u;
    for(uint32_t i=at;i<n;++i) {
        char c=p[i];
        if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;
        if(c=='.' && (i==at || p[i-1u]=='.'))return 0;
    }
    if(p[n-1u]=='.')return 0;
    if(n>=4u && job_same(p+n-4u,".wlm",4u))return 0;
    if(n>=5u && job_same(p+n-5u,".flag",5u))return 0;
    return 1;
}
static uint32_t job_classify(const Job *j,const char *p,uint32_t n,uint32_t *ext_bytes,const char **ext) {
    if(job_private(j,p,n))return PATH_PRIVATE;
    *ext=job_photo(p,n,ext_bytes);
    return *ext && j->photos?PATH_PHOTO:PATH_DENY;
}
static int job_copy_path(const volatile char *from,char out[128],uint32_t *n) {
    uint32_t i=0;
    for(;i<127u;++i){out[i]=from[i];if(!out[i])break;}
    if(i==127u || !i)return 0;
    *n=i;for(uint32_t k=i;k<128u;++k)out[k]=0;
    return 1;
}
static int job_created(const Job *j,const char *path,uint32_t n) {
    if(n>=48u)return -1;
    for(uint32_t i=0;i<FX_JOB_MAX_FILES;++i)if(j->created[i][0] && job_same(j->created[i],path,n+1u))return (int)i;
    return -1;
}
static void job_remember(Job *j,const char *path,uint32_t n) {
    if(n>=48u)return;
    for(uint32_t i=0;i<FX_JOB_MAX_FILES;++i)if(!j->created[i][0]){memcpy(j->created[i],path,n+1u);return;}
}
/* ---- large memory ---- */
static int job_identity_rw(uintptr_t va) {
    uint64_t par;__asm__ volatile("at s1e1w,%1; isb; mrs %0,par_el1":"=r"(par):"r"(va):"memory");
    return !(par&1u) && (par&FX_EXEC_PA_MASK)==(uint64_t)va && (par>>56)==255u;
}
static void job_cache(uintptr_t a,uint32_t bytes,int invalidate) {
    uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
    uintptr_t line=(uintptr_t)4u<<((ctr>>16)&15u),end=a+bytes;
    for(uintptr_t at=a&~(line-1u);at<end;at+=line) {
        if(invalidate)__asm__ volatile("dc civac,%0"::"r"(at):"memory");
        else __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
}
static FxResult job_map(Job *j,uintptr_t base,uint32_t bytes) {
    FxZ8ModuleSession *s=j->session;uint64_t *l1=s->user_tables[0];
    uintptr_t pool=s->base+s->layout.extra+JOB_L2;
    const uint64_t attributes=UINT64_C(1)|(UINT64_C(3)<<8)|(UINT64_C(1)<<10)|(UINT64_C(1)<<6)|FX_EXEC_UXN|FX_EXEC_PXN;
    for(uintptr_t va=base;va<base+bytes;va+=BLOCK2M) {
        uint64_t *e1=&l1[(va>>30)&511u];
        if(!*e1) {
            if(j->l2_used>=JOB_L2_TABLES)return FX_BUDGET;
            uintptr_t table=pool+(uintptr_t)j->l2_used++*4096u;
            memset((void *)table,0,4096u);__asm__ volatile("dsb ishst":::"memory");
            *e1=(uint64_t)table|3u;
        }
        if((*e1&3u)!=3u)return FX_INTEGRITY;
        uint64_t *l2=(uint64_t *)(uintptr_t)(*e1&FX_EXEC_PA_MASK);
        uint64_t *e2=&l2[(va>>21)&511u];
        if(*e2)return FX_INTEGRITY; /* A shared 2 MiB region is never widened. */
        *e2=(uint64_t)va|attributes;
    }
    __asm__ volatile("dsb ish; isb":::"memory");
    return FX_OK;
}
static void job_unmap(Job *j,uintptr_t base,uint32_t bytes) {
    uint64_t *l1=j->session->user_tables[0];
    for(uintptr_t va=base;va<base+bytes;va+=BLOCK2M) {
        uint64_t e1=l1[(va>>30)&511u];
        if((e1&3u)==3u)((uint64_t *)(uintptr_t)(e1&FX_EXEC_PA_MASK))[(va>>21)&511u]=0u;
    }
    /* Another CPU may be in the main entry at EL0 with this region cached. */
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb":::"memory");
}
static FxResult job_alloc(Job *j,uint64_t requested,uint64_t *address) {
    if(!requested || requested>FX_JOB_BIG_MAX)return FX_BOUNDS;
    uint32_t bytes=(uint32_t)((requested+BLOCK2M-1u)&~(uint64_t)(BLOCK2M-1u));
    if(j->memory_bytes+bytes>FX_JOB_BIG_BUDGET)return FX_BUDGET;
    JobBuffer *b=0;
    for(uint32_t i=0;i<FX_JOB_MAX_BUFFERS;++i)if(!j->buffers[i].used){b=&j->buffers[i];break;}
    if(!b)return FX_BUDGET;
    /* Leave the camera a known contiguous block: failure here is a clean refusal. */
    void *floor=ALLOCATE(FX_JOB_FLOOR);
    if(!floor)return FX_BUDGET;
    void *raw=ALLOCATE(bytes+BLOCK2M);
    RELEASE(floor);
    if(!raw)return FX_BUDGET;
    uintptr_t base=((uintptr_t)raw+BLOCK2M-1u)&~(uintptr_t)(BLOCK2M-1u);
    for(uintptr_t va=base;va<base+bytes;va+=65536u)
        if(!job_identity_rw(va)){RELEASE(raw);return FX_CAPABILITY;}
    /* Reused native heap may hold camera data: EL0 must only see zeroes. */
    memset((void *)base,0,bytes);
    FxResult r=job_map(j,base,bytes);
    if(r!=FX_OK){job_unmap(j,base,bytes);RELEASE(raw);return r;}
    *b=(JobBuffer){raw,base,bytes,1u};
    j->memory_bytes+=bytes;if(j->memory_bytes>j->memory_peak)j->memory_peak=j->memory_bytes;
    *address=base;return FX_OK;
}
static FxResult job_free(Job *j,uint64_t address) {
    for(uint32_t i=0;i<FX_JOB_MAX_BUFFERS;++i) {
        JobBuffer *b=&j->buffers[i];
        if(b->used && b->base==address) {
            job_unmap(j,b->base,b->bytes);RELEASE(b->raw);
            j->memory_bytes-=b->bytes;*b=(JobBuffer){0};return FX_OK;
        }
    }
    return FX_STALE;
}
static int job_range(const Job *j,uint64_t address,uint64_t bytes) {
    if(!bytes || address>UINT64_MAX-bytes)return 0;
    for(uint32_t i=0;i<FX_JOB_MAX_BUFFERS;++i) {
        const JobBuffer *b=&j->buffers[i];
        if(b->used && address>=b->base && address+bytes<=b->base+b->bytes)return 1;
    }
    return 0;
}
/* ---- card IO ---- */
static JobFile *job_file(Job *j,uint64_t handle) {
    if(!handle || handle>FX_JOB_MAX_FILES)return 0;
    JobFile *f=&j->files[handle-1u];return f->used?f:0;
}
static int64_t job_open(Job *j,uint64_t mode,uint64_t *second) {
    char path[128];uint32_t n=0u,e=0u;const char *ext=0;
    if(mode>FX_JOB_OPEN_CREATE)return -(int64_t)FX_ARGUMENT;
    if(!job_copy_path(j->shared->path,path,&n))return -(int64_t)FX_ARGUMENT;
    uint32_t kind=job_classify(j,path,n,&e,&ext);
    if(kind==PATH_DENY)return -(int64_t)FX_CAPABILITY;
    if(kind==PATH_PHOTO) {
        if(mode==FX_JOB_OPEN_READ && !(job_ext(ext,e,"NEF")||job_ext(ext,e,"NEFX")||job_ext(ext,e,"JPG")||job_ext(ext,e,"HIF")||job_ext(ext,e,"TMP")))
            return -(int64_t)FX_CAPABILITY;
        if(mode==FX_JOB_OPEN_CREATE && !job_ext(ext,e,"TMP"))return -(int64_t)FX_CAPABILITY;
    }
    JobFile *f=0;uint32_t index=0u;
    for(;index<FX_JOB_MAX_FILES;++index)if(!j->files[index].used){f=&j->files[index];break;}
    if(!f)return -(int64_t)FX_BUDGET;
    int32_t fd=CARD_OPEN(path,mode==FX_JOB_OPEN_READ?0u:0x101u,0u);
    if(fd<0) {
        *second=(uint32_t)fd;j->native_error=(uint32_t)fd;
        return -(int64_t)((uint32_t)fd==UINT32_C(0x80000010)?FX_EMPTY:FX_IO);
    }
    uint64_t size=0u;
    if(mode==FX_JOB_OPEN_READ) {
        if(!CARD_SEEK(fd,0,2u,&size) || !CARD_SEEK(fd,0,0u,&(uint64_t){0})) {
            (void)CARD_CLOSE(fd);return -(int64_t)FX_IO;
        }
    } else job_remember(j,path,n);
    *f=(JobFile){fd,1u,mode==FX_JOB_OPEN_CREATE,0u,0u,size};
    *second=size;return (int64_t)index+1;
}
static void job_beat(Job *j){j->last_beat_ms=job_now();}
static int64_t job_transfer(Job *j,uint64_t handle,uint64_t offset,uint64_t address,uint64_t bytes,int write,uint64_t *second) {
    JobFile *f=job_file(j,handle);
    if(!f)return -(int64_t)FX_STALE;
    if(f->uncertain)return -(int64_t)FX_CLOSE;
    if(write!=(int)f->create)return -(int64_t)FX_ARGUMENT;
    if(bytes>FX_JOB_BIG_MAX || !job_range(j,address,bytes))return -(int64_t)FX_BOUNDS;
    if(offset>INT64_MAX)return -(int64_t)FX_BOUNDS;
    if(f->position!=offset) {
        uint64_t position=0u;
        if(!CARD_SEEK(f->fd,(int64_t)offset,0u,&position) || position!=offset)return -(int64_t)FX_IO;
        f->position=offset;
    }
    uint64_t total=0u;
    while(total<bytes) {
        if(atomic_load_explicit(&j->cancel,memory_order_acquire)>1u)return -(int64_t)FX_INTERRUPTED;
        uint32_t chunk=bytes-total>FX_JOB_IO_CHUNK?FX_JOB_IO_CHUNK:(uint32_t)(bytes-total),done=0u,error;
        void *at=(uint8_t *)(uintptr_t)address+total;
        if(j->inject_after && total+chunk>=j->inject_after) {
            j->inject_after=0u;*second=total;j->native_error=UINT32_C(0xdead0001);
            f->position=UINT64_MAX;return -(int64_t)FX_IO;
        }
        if(write) {
            job_cache((uintptr_t)at,chunk,0);
            error=CARD_WRITE(f->fd,at,chunk,&done,0);
        } else {
            job_cache((uintptr_t)at,chunk,1);
            error=CARD_READ(f->fd,at,chunk,&done);
            job_cache((uintptr_t)at,chunk,1);
        }
        if(done>chunk){f->uncertain=1u;return -(int64_t)FX_INTEGRITY;}
        total+=done;f->position+=done;
        if(write)j->write_bytes+=done;else j->read_bytes+=done;
        job_beat(j);
        if(error){j->native_error=error;*second=error;f->position=UINT64_MAX;return -(int64_t)FX_IO;}
        if(!done || done<chunk)break;
    }
    return (int64_t)total;
}
static FxResult job_close(Job *j,uint64_t handle,uint64_t *second) {
    JobFile *f=job_file(j,handle);
    if(!f)return FX_STALE;
    uint32_t r=CARD_CLOSE(f->fd);
    if(r){*second=r;j->native_error=r;f->uncertain=1u;return FX_CLOSE;}
    *f=(JobFile){0};return FX_OK;
}
static int64_t job_remove(Job *j,uint64_t *second) {
    char path[128];uint32_t n=0u,e=0u;const char *ext=0;
    if(!job_copy_path(j->shared->path,path,&n))return -(int64_t)FX_ARGUMENT;
    uint32_t kind=job_classify(j,path,n,&e,&ext);int at=-1;
    if(kind==PATH_DENY)return -(int64_t)FX_CAPABILITY;
    /* Photos: only a file this job created, never an existing picture. */
    if(kind==PATH_PHOTO){at=job_created(j,path,n);if(at<0)return -(int64_t)FX_CAPABILITY;}
    uint32_t r=CARD_REMOVE(path);
    if(r){*second=r;j->native_error=r;return -(int64_t)FX_IO;}
    if(at>=0)j->created[at][0]=0;
    return 0;
}
static int64_t job_rename(Job *j,uint64_t *second) {
    char from[128],to[128];uint32_t n=0u,m=0u,e=0u,f=0u;const char *ext=0,*fext=0;
    if(!job_copy_path(j->shared->path,from,&n) || !job_copy_path(j->shared->path2,to,&m))return -(int64_t)FX_ARGUMENT;
    uint32_t a=job_classify(j,from,n,&f,&fext),b=job_classify(j,to,m,&e,&ext);int at=-1;
    if(a==PATH_DENY || a!=b)return -(int64_t)FX_CAPABILITY;
    if(a==PATH_PHOTO) {
        at=job_created(j,from,n);
        /* Own temporary to final NEFX in the same folder; never NEF/JPG. */
        uint32_t split=n;while(split && from[split-1u]!='\\')--split;
        if(at<0 || !job_ext(ext,e,"NEFX") || m<split || !job_same(from,to,split))return -(int64_t)FX_CAPABILITY;
    }
    uint32_t r=CARD_RENAME(from,to);
    if(r){*second=r;j->native_error=r;return -(int64_t)FX_IO;}
    if(at>=0)memcpy(j->created[at],to,m+1u);
    return 0;
}
static int64_t job_list_open(Job *j,uint64_t *second) {
    char path[128];uint32_t n=0u;
    if(!job_copy_path(j->shared->path,path,&n))return -(int64_t)FX_ARGUMENT;
    if(!j->photos || j->dir_open)return -(int64_t)(j->dir_open?FX_BUSY:FX_CAPABILITY);
    if(n<7u || (path[0]!='A' && path[0]!='B') || !job_same(path+1,":\\DCIM",6u))return -(int64_t)FX_CAPABILITY;
    if(n>7u) {
        if(path[7]!='\\' || n>16u)return -(int64_t)FX_CAPABILITY;
        for(uint32_t i=8u;i<n;++i)if(!job_upper(path[i]))return -(int64_t)FX_CAPABILITY;
    }
    int32_t fd=-1;uint32_t r=DIR_OPEN(path,&fd);
    if(r || fd<0){*second=r;return -(int64_t)(r==UINT32_C(0x80000010)?FX_EMPTY:FX_IO);}
    j->dir=fd;j->dir_open=1u;return 0;
}
static int64_t job_list_next(Job *j) {
    if(!j->dir_open)return -(int64_t)FX_STATE;
    uint8_t data[0x220] __attribute__((aligned(8)))={0};
    uint32_t r=DIR_NEXT(j->dir,data);
    if(r==UINT32_C(0x80000030))return -(int64_t)FX_EMPTY;
    if(r)return -(int64_t)FX_IO;
    FxJobEntry e={0};
    uint32_t i=0u;
    for(;i<39u;++i){uint32_t ch=(uint32_t)data[14u+i*2u]|(uint32_t)data[15u+i*2u]<<8;if(!ch)break;e.name[i]=ch<128u?(char)ch:'?';}
    if(!i)for(;i<13u && data[i];++i)e.name[i]=(char)data[i];
    e.attributes=data[0x20e];
    e.bytes=(uint64_t)data[0x218]|(uint64_t)data[0x219]<<8|(uint64_t)data[0x21a]<<16|(uint64_t)data[0x21b]<<24|
        (uint64_t)data[0x21c]<<32|(uint64_t)data[0x21d]<<40|(uint64_t)data[0x21e]<<48|(uint64_t)data[0x21f]<<56;
    memcpy((void *)&j->shared->entry,&e,sizeof(e));
    return 0;
}
static void job_syscall(Job *j) {
    uint64_t *c=j->exec.continuation,op=c[0],a=c[1],b=c[2],d=c[3],e=c[4],second=0u;int64_t r;
    ++j->syscalls;
    switch(op) {
    case FX_SYS_ALLOC: {uint64_t address=0u;FxResult x=job_alloc(j,a,&address);r=x==FX_OK?(int64_t)address:-(int64_t)x;break;}
    case FX_SYS_FREE: r=-(int64_t)job_free(j,a);break;
    case FX_SYS_OPEN: r=job_open(j,a,&second);break;
    case FX_SYS_READ_AT: r=job_transfer(j,a,b,d,e,0,&second);break;
    case FX_SYS_WRITE_AT: r=job_transfer(j,a,b,d,e,1,&second);break;
    case FX_SYS_CLOSE: r=-(int64_t)job_close(j,a,&second);break;
    case FX_SYS_REMOVE: r=job_remove(j,&second);break;
    case FX_SYS_RENAME: r=job_rename(j,&second);break;
    case FX_SYS_LIST_OPEN: r=job_list_open(j,&second);break;
    case FX_SYS_LIST_NEXT: r=job_list_next(j);break;
    case FX_SYS_LIST_CLOSE:
        if(!j->dir_open){r=-(int64_t)FX_STATE;break;}
        (void)DIR_CLOSE(j->dir);j->dir_open=0u;r=0;break;
    case FX_SYS_TIME: r=(int64_t)job_now();break;
    case FX_SYS_SLEEP:
        r=a>=1u && a<=1000u && fx_z8_task_delay_ticks((uint32_t)a)==FX_OK?0:-(int64_t)FX_ARGUMENT;break;
#ifdef FX_JOB_TEST
    case FX_SYS_TEST_INJECT: j->inject_after=(uint32_t)a;r=0;break;
#endif
    default: r=-(int64_t)FX_ARGUMENT;break;
    }
    job_beat(j);
    c[0]=(uint64_t)r;c[1]=second;
}
/* ---- thread ---- */
static int job_operation(FxZ8ExecContext *context) {
    Job *j=(Job *)context;FxZ8ModuleSession *s=j->session;
    j->module_result=fx_z8_user_call((uintptr_t)j->entry,j->api,s->user_root,j->stack_top,&j->exec);
    return context->continuation[34]?(int)FX_INTERRUPTED:(int)FX_OK;
}
static void job_release_all(Job *j) {
    for(uint32_t i=0;i<FX_JOB_MAX_FILES;++i)if(j->files[i].used && !j->files[i].uncertain) {
        uint64_t ignored=0u;(void)job_close(j,i+1u,&ignored);
    }
    if(j->dir_open){(void)DIR_CLOSE(j->dir);j->dir_open=0u;}
}
static void job_finish(Job *j,uint32_t state,FxResult error) {
    /* Close files in all cases. Faulted/cancelled-hard buffers are retained until
     * reboot; a completed job has returned from EL0 and its buffers can go. */
    job_release_all(j);
    if(state==FX_JOB_DONE || state==FX_JOB_CANCELLED)
        for(uint32_t i=0;i<FX_JOB_MAX_BUFFERS;++i)if(j->buffers[i].used)(void)job_free(j,j->buffers[i].base);
    j->elapsed_ms=job_now()-j->begin_ms;
    atomic_store_explicit(&j->error,(uint32_t)error,memory_order_release);
    atomic_store_explicit(&j->state,state,memory_order_release);
    if(state==FX_JOB_FAILED)fx_platform_wake(j->owner);
}
static void job_run(Job *j) {
    FxZ8ModuleSession *s=j->session;
    j->begin_ms=job_now();j->last_beat_ms=j->begin_ms;j->last_heartbeat=j->shared->heartbeat;j->cancel_ms=0u;
    j->exec.simd_enabled=s->exec.simd_enabled;j->exec.continuation[34]=0u;
    uint64_t slice=j->begin_ms;
    atomic_store_explicit(&j->state,FX_JOB_RUNNING,memory_order_release);
    for(;;) {
        uint32_t cancel=atomic_load_explicit(&j->cancel,memory_order_acquire);
        uint64_t now=job_now();
        j->api->cancel=cancel?1u:0u;
        if(cancel>1u || s->closed){job_finish(j,cancel>2u || s->closed?FX_JOB_FAILED:FX_JOB_CANCELLED,cancel>2u || s->closed?FX_STATE:FX_OK);return;}
        if(cancel==1u) {
            if(!j->cancel_ms)j->cancel_ms=now;
            else if(now-j->cancel_ms>FX_JOB_SOFT_STOP_MS){atomic_store_explicit(&j->cancel,2u,memory_order_release);continue;}
        }
        if(now-j->begin_ms>j->start.total_ms){job_finish(j,FX_JOB_FAILED,FX_TIMEOUT);return;}
        uint64_t beat=j->shared->heartbeat;
        if(beat!=j->last_heartbeat){j->last_heartbeat=beat;j->last_beat_ms=now;}
        else if(now-j->last_beat_ms>j->start.watchdog_ms){job_finish(j,FX_JOB_FAILED,FX_TIMEOUT);return;}
        FxResult r=fx_z8_exec_call(&j->exec,s->private_root,job_operation);
        if(r==FX_OK) {
            j->result=(uint64_t)(int64_t)j->module_result;
            job_finish(j,FX_JOB_DONE,FX_OK);return;
        }
        if(r==FX_YIELD)job_syscall(j);
        else if(r==FX_INTERRUPTED)++j->resumes;
        else {
            s->job_faulted=1u;
            job_finish(j,FX_JOB_FAILED,r);return;
        }
        if(j->start.run_ms && job_now()-slice>=j->start.run_ms) {
            ++j->sleeps;(void)fx_z8_task_delay_ticks(j->start.sleep_ticks);slice=job_now();
        }
    }
}
static void job_task(void *context) {
    Job *j=context;
    for(;;) {
        if(atomic_load_explicit(&j->state,memory_order_acquire)==FX_JOB_QUEUED)job_run(j);
        else (void)fx_z8_task_delay_ticks(5u);
    }
}
static Job *job_of(FxZ8ModuleSession *s){return (Job *)(s->base+s->layout.extra+JOB_CONTEXT);}
FxResult fx_z8_session_job_bind(FxZ8ModuleSession *s,FxPlatformOwner *owner,const char *id,uint32_t capabilities) {
    if(!s || !owner || !id)return FX_ARGUMENT;
    Job *j=job_of(s);uintptr_t base=s->base+s->layout.extra;
    j->session=s;j->owner=owner;
    j->api=(FxJobApi *)(base+JOB_API);j->shared=(FxJobShared *)(base+JOB_SHARED);
    j->stack_top=base+JOB_USER+JOB_USER_BYTES;
    j->photos=(capabilities&FX_SDK_CARD_PHOTOS)?1u:0u;
    uint32_t n=0u;while(n<15u && id[n]){j->module_id[n]=id[n];++n;}
    /* The .wlm stem is the id; files under it can never collide with another owner. */
    if(n>=4u && job_same(id+n-4u,".wlm",4u))j->module_id[n-4u]=0;
    owner->native_session=s;
    return FX_OK;
}
FxResult fx_z8_session_job_fault(FxZ8ModuleSession *s) {
    if(!s || !s->layout.extra)return FX_OK;
    Job *j=job_of(s);
    if(!j->session || atomic_load_explicit(&j->state,memory_order_acquire)!=FX_JOB_FAILED)return FX_OK;
    return (FxResult)atomic_load_explicit(&j->error,memory_order_acquire);
}
void fx_z8_session_job_abort(FxZ8ModuleSession *s) {
    if(!s || !s->layout.extra)return;
    Job *j=job_of(s);
    if(j->session)atomic_store_explicit(&j->cancel,3u,memory_order_release);
}
const FxZ8ExecDiagnostics *fx_z8_session_job_diagnostics(const FxZ8ModuleSession *s) {
    if(!s || !s->job_faulted)return 0;
    return &job_of((FxZ8ModuleSession *)s)->exec.diagnostics;
}
static int job_entry_valid(const FxZ8ModuleSession *s,uint64_t entry) {
    uintptr_t image=(uintptr_t)s->image;
    if((entry&3u) || entry<image || entry>=image+s->layout.image_bytes)return 0;
    uint64_t pte=s->arena.expected[(entry-image)/4096u];
    return pte && !(pte&FX_EXEC_PXN);
}
FxResult fx_z8_jobs_resource(void *context,uint32_t op,const FxNamedBinding *b,FxPlatformOwner *owner,uint64_t token,
    uint64_t *cookie,const FxServiceRequest *q,FxServiceReply *a) {
    (void)context;(void)b;(void)token;
    FxZ8ModuleSession *s=owner?owner->native_session:0;
    if(!s || !cookie)return FX_STATE;
    Job *j=job_of(s);
    if(op==FX_RESOURCE_ATTACH){*cookie=1u;return FX_OK;}
    if(op==FX_RESOURCE_PANIC){atomic_store_explicit(&j->cancel,3u,memory_order_release);return FX_OK;}
    if(op==FX_RESOURCE_DETACH) {
        if(job_active(j)) {
            uint32_t none=0u;
            (void)atomic_compare_exchange_strong(&j->cancel,&none,1u);
            return FX_BUSY;
        }
        *cookie=0u;return FX_OK;
    }
    if(op!=FX_RESOURCE_CALL || !q || !a || !*cookie || q->arg[3])return FX_ARGUMENT;
    if(q->arg[2]==FX_JOB_QUERY) {
        a->value[0]=FX_JOB_ABI;a->value[1]=FX_JOB_MAX_BUFFERS;a->value[2]=FX_JOB_BIG_MAX;a->value[3]=FX_JOB_MAX_FILES;
        return FX_OK;
    }
    if(q->arg[2]==FX_JOB_STATUS) {
        FxJobStatus status={FX_JOB_ABI,sizeof(status),atomic_load_explicit(&j->state,memory_order_acquire),
            atomic_load_explicit(&j->error,memory_order_acquire),j->result,j->shared?j->shared->progress:0u,
            job_active(j)?job_now()-j->begin_ms:j->elapsed_ms,j->resumes,j->syscalls,j->sleeps,j->read_bytes,j->write_bytes,
            j->memory_bytes,j->memory_peak,j->native_error,atomic_load_explicit(&j->cancel,memory_order_acquire)};
        memcpy(a->data,&status,sizeof(status));a->bytes=sizeof(status);return FX_OK;
    }
    if(q->arg[2]==FX_JOB_CANCEL) {
        if(!job_active(j))return FX_STATE;
        uint32_t none=0u;(void)atomic_compare_exchange_strong(&j->cancel,&none,1u);
        return FX_OK;
    }
    if(q->arg[2]!=FX_JOB_START || q->bytes!=sizeof(FxJobStart))return FX_ARGUMENT;
    FxJobStart start;memcpy(&start,q->data,sizeof(start));
    if(start.abi!=FX_JOB_ABI || start.bytes!=sizeof(start) || start.reserved || !job_entry_valid(s,start.entry) ||
       start.total_ms>1800000u || start.watchdog_ms>600000u || start.run_ms>1000u || start.sleep_ticks>100u ||
       (start.priority && (start.priority<20u || start.priority>60u)))return FX_ARGUMENT;
    uint32_t state=atomic_load_explicit(&j->state,memory_order_acquire);
    if(job_active(j))return FX_BUSY;
    if(state==FX_JOB_FAILED)return FX_STATE;
    if(!start.total_ms)start.total_ms=1800000u;
    if(!start.watchdog_ms)start.watchdog_ms=10000u;
    if(!start.sleep_ticks)start.sleep_ticks=1u;
    j->start=start;j->entry=start.entry;j->result=0u;
    j->api->abi=FX_JOB_ABI;j->api->bytes=sizeof(*j->api);j->api->argument=start.argument;j->api->cancel=0u;
    j->api->photos=j->photos;j->api->shared=j->shared;
    atomic_store_explicit(&j->cancel,0u,memory_order_release);
    j->resumes=j->sleeps=j->syscalls=j->read_bytes=j->write_bytes=j->memory_peak=0u;
    atomic_store_explicit(&j->state,FX_JOB_QUEUED,memory_order_release);
    if(!atomic_exchange_explicit(&j->started,1u,memory_order_acq_rel)) {
        FxResult r=fx_z8_task_start_priority(&j->task,job_task,j,
            (void *)(s->base+s->layout.extra+JOB_NATIVE),JOB_NATIVE_BYTES,start.priority?start.priority:FX_JOB_PRIORITY);
        if(r!=FX_OK){atomic_store_explicit(&j->state,FX_JOB_FAILED,memory_order_release);atomic_store_explicit(&j->error,(uint32_t)r,memory_order_release);return r;}
    }
    return FX_OK;
}
