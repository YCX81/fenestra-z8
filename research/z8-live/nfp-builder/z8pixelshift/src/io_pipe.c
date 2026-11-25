/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One compute producer, one FS owner. All communication uses acquire/release.
   Slot buffers belong to the pipe until join; synchronous RPC borrowed pointers
   remain live because waits have no timeout or early cancellation return. */
#include "io_pipe.h"
#define IO_FILES 40u
#define IO_READS 8u
#define IO_READ_BYTES 0x40000u
#define IO_WRITE_BYTES 0x80000u
#define IO_SLOTS (IO_READS+3u)
#define IO_PHYSICAL 16u
enum {IO_FREE,IO_PENDING,IO_DONE};
enum {IO_OPEN=1,IO_READ,IO_CREATE,IO_WRITE,IO_CLOSE,IO_EXISTS,IO_RENAME,IO_REMOVE,IO_STOP};
typedef struct {
    uint32_t state,op,seq,offset,bytes;
    int fd,result;
    uint32_t size;
    void *data;
    char path[128],other[128];
} io_request;
typedef struct {char path[128];uint32_t active,bytes,output,stamp;int fd;} io_file;
struct ps8_io {
    psm_platform base,proxy;
    void *read_memory,*write_memory;
    uint8_t *read_buffers,*write_buffers;
    io_request q[IO_SLOTS]; /* 0 RPC, 1..2 writes, 3..10 shared bounded prefetch pool */
    io_file files[IO_FILES];
    uint32_t frames,read_slots,write_bytes,sequence,stamp,ready,fatal,stopped;
    uint32_t hits,misses,high;
    uint64_t service,input_wait,output_wait;
};
static uint32_t io_load(const uint32_t *v){return __atomic_load_n(v,__ATOMIC_ACQUIRE);}
static void io_store(uint32_t *v,uint32_t x){__atomic_store_n(v,x,__ATOMIC_RELEASE);}
static void io_error(ps8_io *s,uint32_t status){
    uint32_t expected=0;__atomic_compare_exchange_n(&s->fatal,&expected,status,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static void io_zero(void *p,size_t n){uint8_t *b=p;while(n--)*b++=0;}
static void io_copy(void *p,const void *s,size_t n){
    typedef uint64_t word __attribute__((may_alias));
    uint8_t *d=p;const uint8_t *b=s;
    if(!(((uintptr_t)d|(uintptr_t)b)&7u))while(n>=8){*(word *)d=*(const word *)b;d+=8;b+=8;n-=8;}
    while(n--)*d++=*b++;
}
static int io_path(char *out,const char *in){
    unsigned i=0;for(;i<127&&in[i];++i)out[i]=in[i];out[i]=0;return in[i]!=0;
}
static uint64_t io_time(ps8_io *s){return s->base.ticks?s->base.ticks(s->base.context):0;}
static void io_wake(ps8_io *s,uint32_t who){s->base.io_wake(s->base.context,who);}
static void io_wait_done(ps8_io *s,io_request *q){
    while(io_load(&q->state)!=IO_DONE)s->base.io_wait(s->base.context,0);
}
static void io_enqueue(ps8_io *s,io_request *q){
    q->seq=++s->sequence;io_store(&q->state,IO_PENDING);
    uint32_t n=0;for(unsigned i=0;i<IO_SLOTS;++i)n+=io_load(&s->q[i].state)==IO_PENDING;
    if(n>s->high)s->high=n;io_wake(s,1);
}
/* Worker-only LRU. Logical input identities stay open while <=16 physical
   handles are open; every reopening checks the length against initial open. */
static int io_room(ps8_io *s){
    uint32_t n=0,old=UINT32_MAX;int victim=-1;
    for(unsigned i=0;i<IO_FILES;++i){io_file *f=&s->files[i];
        if(f->active&&!f->output&&f->fd>=0){++n;if(f->stamp<old){old=f->stamp;victim=(int)i;}}
    }
    if(n<IO_PHYSICAL)return 0;
    io_file *f=&s->files[victim];
    if(s->base.close(s->base.context,f->fd))return 1;f->fd=-1;return 0;
}
static int io_fd(ps8_io *s,int index){
    if(index<0||index>=(int)IO_FILES||!s->files[index].active)return -1;
    io_file *f=&s->files[index];
    if(f->fd<0){
        uint32_t bytes=0;if(f->output||io_room(s))return -1;
        f->fd=s->base.open(s->base.context,f->path,&bytes);
        if(f->fd<0)return -1;
        if(bytes!=f->bytes){s->base.close(s->base.context,f->fd);f->fd=-1;return -1;}
    }
    f->stamp=++s->stamp;return f->fd;
}
static void io_execute(ps8_io *s,io_request *q){
    const psm_platform *b=&s->base;int fd;
    q->result=1;
    switch(q->op){
    case IO_OPEN:case IO_CREATE:
        q->result=-1;
        for(unsigned i=0;i<IO_FILES;++i)if(!s->files[i].active){
            io_file *f=&s->files[i];uint32_t bytes=0;
            if(q->op==IO_OPEN&&io_room(s))break;
            fd=q->op==IO_OPEN?b->open(b->context,q->path,&bytes):b->create(b->context,q->path);
            if(fd<0)break;
            f->fd=fd;f->bytes=bytes;f->active=1;f->output=q->op==IO_CREATE;f->stamp=++s->stamp;
            io_path(f->path,q->path);q->size=bytes;q->result=(int)i;break;
        }
        break;
    case IO_READ:
        fd=io_fd(s,q->fd);if(fd>=0)q->result=b->read_at(b->context,fd,q->offset,q->data,q->bytes);
        if(q->result)io_error(s,PSM_READ_ERROR);break;
    case IO_WRITE:
        fd=io_fd(s,q->fd);if(fd>=0&&!io_load(&s->fatal))q->result=b->write_at(b->context,fd,q->offset,q->data,q->bytes);
        if(q->result)io_error(s,PSM_WRITE_ERROR);break;
    case IO_CLOSE:
        if(q->fd>=0&&q->fd<(int)IO_FILES&&s->files[q->fd].active){
            io_file *f=&s->files[q->fd];
            q->result=f->fd<0?0:b->close(b->context,f->fd);
            if(!q->result){f->fd=-1;f->active=0;}
            if(f->output&&io_load(&s->fatal))q->result=1;
        }break;
    case IO_EXISTS:q->result=b->exists(b->context,q->path);break;
    case IO_RENAME:q->result=io_load(&s->fatal)?1:b->rename(b->context,q->path,q->other);break;
    case IO_REMOVE:q->result=b->remove(b->context,q->path);break;
    case IO_STOP:
        q->result=0;
        for(unsigned i=0;i<IO_FILES;++i){io_file *f=&s->files[i];
            if(f->active&&f->fd>=0){
                int st=b->close(b->context,f->fd);if(st)st=b->close(b->context,f->fd);
                if(st)q->result=1;else{f->fd=-1;f->active=0;}
            }
        }break;
    }
}
static void io_worker(void *arg){
    ps8_io *s=arg;io_store(&s->ready,1);io_wake(s,0);
    for(;;){
        io_request *next=0;uint32_t seq=UINT32_MAX;
        for(unsigned i=0;i<IO_SLOTS;++i)if(io_load(&s->q[i].state)==IO_PENDING&&s->q[i].seq<seq){next=&s->q[i];seq=next->seq;}
        if(!next){s->base.io_wait(s->base.context,1);continue;}
        uint64_t t=io_time(s);io_execute(s,next);s->service+=io_time(s)-t;
        int stop=next->op==IO_STOP;io_store(&next->state,IO_DONE);io_wake(s,0);if(stop)return;
    }
}
static int io_rpc(ps8_io *s,uint32_t op,int fd,uint32_t off,void *data,uint32_t bytes,const char *path,const char *other,uint32_t *size){
    io_request *q=&s->q[0];q->op=op;q->fd=fd;q->offset=off;q->data=data;q->bytes=bytes;
    if(path&&io_path(q->path,path))return op==IO_OPEN||op==IO_CREATE?-1:1;
    if(other&&io_path(q->other,other))return 1;
    io_enqueue(s,q);io_wait_done(s,q);if(size)*size=q->size;
    int result=q->result;io_store(&q->state,IO_FREE);return result;
}
static void *ip_alloc(void *c,uint32_t n){ps8_io *s=c;return s->base.allocate(s->base.context,n);}
static void ip_free(void *c,void *p){ps8_io *s=c;s->base.release(s->base.context,p);}
static int ip_open(void *c,const char *p,uint32_t *n){return io_rpc(c,IO_OPEN,0,0,0,0,p,0,n);}
static int ip_create(void *c,const char *p){return io_rpc(c,IO_CREATE,0,0,0,0,p,0,0);}
static int ip_exists(void *c,const char *p){return io_rpc(c,IO_EXISTS,0,0,0,0,p,0,0);}
static int ip_remove(void *c,const char *p){return io_rpc(c,IO_REMOVE,0,0,0,0,p,0,0);}
static int ip_rename(void *c,const char *p,const char *q){return io_rpc(c,IO_RENAME,0,0,0,0,p,q,0);}
static int ip_close(void *c,int fd){
    ps8_io *s=c;int r=io_rpc(s,IO_CLOSE,fd,0,0,0,0,0,0);
    for(unsigned i=3;i<IO_SLOTS;++i)if(s->q[i].fd==fd)io_store(&s->q[i].state,IO_FREE);
    return r;
}
static int ip_read(void *c,int fd,uint32_t off,void *data,uint32_t bytes){
    ps8_io *s=c;uint64_t t=io_time(s);int r;
    io_request *q=0;
    for(unsigned i=3;i<IO_SLOTS;++i)if(io_load(&s->q[i].state)!=IO_FREE&&s->q[i].fd==fd){q=&s->q[i];break;}
    if(q&&io_load(&q->state)!=IO_FREE&&q->fd==fd&&q->offset==off&&q->bytes==bytes){
        io_wait_done(s,q);r=q->result;if(!r)io_copy(data,q->data,bytes);io_store(&q->state,IO_FREE);++s->hits;
    }else{
        ++s->misses;r=io_rpc(s,IO_READ,fd,off,data,bytes,0,0,0);
        /* A seek invalidates an older completed prefetch, after the FIFO barrier. */
        if(q&&io_load(&q->state)==IO_DONE)io_store(&q->state,IO_FREE);
    }
    s->input_wait+=io_time(s)-t;return r;
}
static void ip_prefetch(void *c,int fd,uint32_t off,uint32_t bytes){
    ps8_io *s=c;if(fd<0||fd>=(int)s->frames||!bytes||bytes>IO_READ_BYTES||io_load(&s->fatal))return;
    io_request *q=0;
    for(unsigned i=3;i<3+s->read_slots;++i){
        if(io_load(&s->q[i].state)!=IO_FREE){if(s->q[i].fd==fd)return;}
        else if(!q)q=&s->q[i];
    }
    if(!q)return;
    q->op=IO_READ;q->fd=fd;q->offset=off;q->bytes=bytes;q->data=s->read_buffers+(q-&s->q[3])*IO_READ_BYTES;io_enqueue(s,q);
}
static int ip_write(void *c,int fd,uint32_t off,const void *data,uint32_t bytes){
    ps8_io *s=c;const uint8_t *src=data;
    while(bytes){
        uint64_t t=io_time(s);io_request *q=0;
        for(;;){
            if(io_load(&s->fatal))return 1;
            for(unsigned i=1;i<=2;++i)if(io_load(&s->q[i].state)!=IO_PENDING){q=&s->q[i];break;}
            if(q)break;s->base.io_wait(s->base.context,0);
        }
        s->output_wait+=io_time(s)-t;
        uint32_t n=bytes<s->write_bytes?bytes:s->write_bytes;
        q->data=s->write_buffers+(q-&s->q[1])*s->write_bytes;io_copy(q->data,src,n);
        q->op=IO_WRITE;q->fd=fd;q->offset=off;q->bytes=n;io_enqueue(s,q);
        src+=n;off+=n;bytes-=n;
    }
    return 0;
}
static int ip_cancelled(void *c){ps8_io *s=c;return s->base.cancelled&&s->base.cancelled(s->base.context);}
static void ip_progress(void *c,uint32_t a,uint32_t b){ps8_io *s=c;if(s->base.progress)s->base.progress(s->base.context,a,b);}
static void ip_phase(void *c,uint32_t a,uint32_t b,uint32_t d){ps8_io *s=c;if(s->base.phase)s->base.phase(s->base.context,a,b,d);}
static uint64_t ip_ticks(void *c){return io_time(c);}
static uint64_t ip_hz(void *c){ps8_io *s=c;return s->base.ticks_per_second?s->base.ticks_per_second(s->base.context):0;}
__attribute__((noinline,used)) void ps8_io_configured(uint32_t frames,uint32_t reads,uint32_t writes,uint32_t fallback){
    __asm__ volatile("" : : "r"(frames),"r"(reads),"r"(writes),"r"(fallback) : "memory");
}
ps8_io *ps8_io_begin(const psm_platform *p,uint32_t frames,uint32_t *fallback){
    *fallback=1;
    if(!p->io_start||!p->io_join||!p->io_wait||!p->io_wake||frames>PSM_MAX_FRAMES||!frames)return 0;
    *fallback=2;
    ps8_io *s=p->allocate(p->context,sizeof(*s));if(!s)return 0;io_zero(s,sizeof(*s));s->base=*p;s->frames=frames;
    *fallback=3;
    /* Preserve the large per-frame decoder caches. On the native heap, an
       eight-slot prefetch pool makes a 32-frame cache fall from 256 to 64 KiB.
       Use one slot for 32 frames, two for 16, and four for 4/8 frames. */
    s->read_slots=frames==32?1u:frames>=16?2u:4u;
    s->read_memory=p->allocate(p->context,s->read_slots*IO_READ_BYTES+64);
    if(!s->read_memory)goto failed;
    *fallback=4;
    /* Select the bounded output queue once, before launching its owner. The
       caller's 512 KiB batch can be split into smaller ordered owned chunks. */
    for(s->write_bytes=IO_WRITE_BYTES;s->write_bytes>=0x20000u;s->write_bytes/=2u){
        s->write_memory=p->allocate(p->context,2*s->write_bytes+64);
        if(s->write_memory)break;
    }
    if(!s->write_memory)goto failed;
    s->read_buffers=(void *)(((uintptr_t)s->read_memory+63u)&~(uintptr_t)63u);
    s->write_buffers=(void *)(((uintptr_t)s->write_memory+63u)&~(uintptr_t)63u);
    for(unsigned i=0;i<IO_FILES;++i)s->files[i].fd=-1;
    psm_platform *b=&s->proxy;*b=*p;b->context=s;b->allocate=ip_alloc;b->release=ip_free;b->open=ip_open;
    b->create=ip_create;b->exists=ip_exists;b->remove=ip_remove;b->rename=ip_rename;b->close=ip_close;b->read_at=ip_read;
    b->write_at=ip_write;b->cancelled=ip_cancelled;b->progress=ip_progress;b->phase=ip_phase;b->ticks=ip_ticks;b->ticks_per_second=ip_hz;
    b->prefetch=ip_prefetch;b->io_mode=1;b->list=0;b->group=0;b->io_start=0;b->io_join=0;b->io_wait=0;b->io_wake=0;
    *fallback=5;if(!p->io_start(p->context,io_worker,s))goto failed;
    while(!io_load(&s->ready))p->io_wait(p->context,0);
    *fallback=0;ps8_io_configured(frames,s->read_slots,s->write_bytes,0);return s;
failed:
    ps8_io_configured(frames,0,0,*fallback);
    if(s->read_memory)p->release(p->context,s->read_memory);
    if(s->write_memory)p->release(p->context,s->write_memory);p->release(p->context,s);return 0;
}
const psm_platform *ps8_io_platform(ps8_io *s){return &s->proxy;}
int ps8_io_end(ps8_io *s,psm_times *t){
    int result=io_rpc(s,IO_STOP,0,0,0,0,0,0,0);s->base.io_join(s->base.context);
    result=io_load(&s->fatal)?(int)io_load(&s->fatal):result?PSM_READ_ERROR:0;
    t->io_mode=1;t->io_service=s->service;t->input_wait=s->input_wait;t->output_wait=s->output_wait;
    t->io_read_slots=s->read_slots;t->io_write_bytes=s->write_bytes;t->prefetch_hits=s->hits;t->prefetch_misses=s->misses;t->queue_high=s->high;
    s->base.release(s->base.context,s->read_memory);s->base.release(s->base.context,s->write_memory);
    psm_platform b=s->base;b.release(b.context,s);return result;
}
