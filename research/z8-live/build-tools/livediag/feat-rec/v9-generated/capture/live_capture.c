#include "live_capture.h"
#define LOAD(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
static void reset(struct la_capture *c) {
    if(c->descriptor && c->ops.release)c->ops.release(c->ops.context,c->descriptor);
    c->descriptor=0;c->pixels=0;c->count=0;c->head=0;c->actual_bytes=0;
    c->stream_start=0;c->press_us=0;STORE(&c->press_mailbox,0);
    STORE(&c->phase,LA_OFF);
}
void la_init(struct la_capture *c,const struct la_ops *ops) {
    unsigned char *p=(unsigned char *)c;
    for(size_t i=0;i<sizeof(*c);i++)p[i]=0;
    c->ops=*ops;
}
void la_request(struct la_capture *c,int enabled) {
    STORE(&c->requested,enabled!=0);
    if(!enabled)la_cancel(c);
}
void la_cancel(struct la_capture *c) {
    __atomic_add_fetch(&c->cancel_epoch,1,__ATOMIC_ACQ_REL);
}
void la_irq_signal(struct la_capture *c,uint32_t event,uint64_t status,uint64_t now) {
    if(event!=0x41070030u || !(status&1))return;
    /* Single native C0 IRQ producer; odd seqlock prevents torn time/gen reads. */
    __atomic_add_fetch(&c->irq_seq,1,__ATOMIC_ACQ_REL);
    STORE(&c->irq_timestamp,now);
    __atomic_add_fetch(&c->irq_seq,1,__ATOMIC_RELEASE);
}
int la_full_press(struct la_capture *c,uint64_t now) {
    uint64_t expected=0;
    if(!now || LOAD(&c->phase)!=LA_READY || !LOAD(&c->requested))return 0;
    return __atomic_compare_exchange_n(&c->press_mailbox,&expected,now,0,
                                       __ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
int la_cancelled(const struct la_capture *c) {
    return !LOAD(&c->requested) || LOAD(&c->cancel_epoch)!=c->worker_epoch;
}
int la_nv12_to_i420(uint8_t *dst,const struct la_source *s) {
    if(!dst || !s->y || !s->uv || s->width<LA_WIDTH || s->height<LA_HEIGHT ||
       s->width>4096 || s->height>4096 || ((s->width|s->height)&1) ||
       s->y_stride<s->width || s->uv_stride<s->width ||
       s->y_stride>8192 || s->uv_stride>8192)return 0;
    uint8_t *u=dst+LA_WIDTH*LA_HEIGHT,*v=u+LA_WIDTH*LA_HEIGHT/4;
    for(uint32_t y=0;y<LA_HEIGHT;y++) {
        const uint8_t *row=s->y+(size_t)(y*s->height/LA_HEIGHT)*s->y_stride;
        for(uint32_t x=0;x<LA_WIDTH;x++)dst[y*LA_WIDTH+x]=row[x*s->width/LA_WIDTH];
    }
    for(uint32_t y=0;y<LA_HEIGHT/2;y++) {
        const uint8_t *row=s->uv+(size_t)(y*(s->height/2)/(LA_HEIGHT/2))*s->uv_stride;
        for(uint32_t x=0;x<LA_WIDTH/2;x++) {
            uint32_t sx=2*(x*(s->width/2)/(LA_WIDTH/2));
            u[y*(LA_WIDTH/2)+x]=row[sx];v[y*(LA_WIDTH/2)+x]=row[sx+1];
        }
    }
    return 1;
}
static void drop_old(struct la_capture *c,uint64_t oldest) {
    while(c->count && c->frames[c->head].timestamp_us<oldest) {
        c->head=(c->head+1)%LA_RING_CAPACITY;c->count--;
    }
}
void la_step(struct la_capture *c,uint64_t now,int eligible) {
    uint32_t epoch=LOAD(&c->cancel_epoch);
    if(!eligible && LOAD(&c->reader_active) && epoch==c->worker_epoch)
        epoch=__atomic_add_fetch(&c->cancel_epoch,1,__ATOMIC_ACQ_REL);
    if(!eligible || !LOAD(&c->requested) || epoch!=c->worker_epoch) {
        c->reason=LA_CANCEL;
        /* Never free encoder-visible pixels until its reader acknowledges. */
        if(LOAD(&c->reader_active))return;
        reset(c);c->worker_epoch=epoch;
        if(!eligible || !LOAD(&c->requested))return;
    }
    if(c->phase==LA_FAILED || c->phase==LA_FROZEN || c->phase==LA_ENCODING)return;
    if(c->phase==LA_OFF) {
        if(!c->ops.allocate || !c->ops.allocate(c->ops.context,LA_RING_BYTES,&c->descriptor,&c->pixels,&c->actual_bytes) || !c->descriptor || !c->pixels || c->actual_bytes<LA_RING_BYTES) {
            c->reason=LA_ALLOC;STORE(&c->phase,LA_FAILED);return;
        }
        STORE(&c->phase,LA_REQUESTED);
    }
    if(c->phase==LA_READY && c->count && now>c->frames[(c->head+c->count-1)%LA_RING_CAPACITY].timestamp_us+LA_MAX_GAP_US) {
        c->count=0;c->stream_start=0;c->reason=LA_GAP;STORE(&c->phase,LA_REQUESTED);
    }
    uint64_t press=__atomic_exchange_n(&c->press_mailbox,0,__ATOMIC_ACQ_REL);
    if(press) {
        if(c->phase==LA_READY && press<=now) {
            c->press_us=press;c->accepted_presses++;
            drop_old(c,press>LA_WINDOW_US?press-LA_WINDOW_US:0);
            STORE(&c->phase,LA_POST);
        } else c->rejected_presses++;
    }
    uint64_t seq=LOAD(&c->irq_seq),stamp=LOAD(&c->irq_timestamp);
    if((seq&1) || seq!=LOAD(&c->irq_seq))return;
    if(seq && seq!=c->last_generation && stamp<=now) {
        c->last_generation=seq;
        if(c->phase==LA_POST && stamp>=c->press_us+LA_WINDOW_US)goto finalize;
        if(now-stamp>LA_MAX_GAP_US){c->dropped++;return;}
        if(c->count) {
            uint64_t last=c->frames[(c->head+c->count-1)%LA_RING_CAPACITY].timestamp_us;
            if(stamp<=last || stamp-last<LA_SAMPLE_INTERVAL_US)return; /* cap at 30 Hz */
            if(stamp-last>LA_MAX_GAP_US) {
                if(c->phase==LA_POST){c->reason=LA_GAP;STORE(&c->phase,LA_FAILED);return;}
                c->count=0;c->stream_start=0;STORE(&c->phase,LA_REQUESTED);
            }
        }
        struct la_source source={0};
        if(!c->ops.lease_begin || !c->ops.lease_valid || !c->ops.lease_end ||
           !c->ops.lease_begin(c->ops.context,seq/2,stamp,&source)) {
            c->reason=LA_SOURCE_UNPROVEN;c->dropped++;return;
        }
        int valid=source.generation==seq/2 && source.timestamp_us==stamp &&
                  c->ops.lease_valid(c->ops.context,&source);
        if(valid && !c->descriptor) {
            valid=c->ops.allocate && c->ops.allocate(c->ops.context,LA_RING_BYTES,
                    &c->descriptor,&c->pixels,&c->actual_bytes) &&
                    c->descriptor && c->pixels && c->actual_bytes>=LA_RING_BYTES;
            if(!valid){c->reason=LA_ALLOC;STORE(&c->phase,LA_FAILED);}
        }
        if(c->phase!=LA_POST) {
            drop_old(c,stamp>LA_WINDOW_US?stamp-LA_WINDOW_US:0);
            /* At most 45 pre frames; keep at least 45 vacant post slots. */
            if(c->count>=LA_RING_CAPACITY/2){c->head=(c->head+1)%LA_RING_CAPACITY;c->count--;}
        }
        uint32_t slot=(c->head+c->count)%LA_RING_CAPACITY;
        if(c->count==LA_RING_CAPACITY) {valid=0;c->reason=LA_OVERFLOW;STORE(&c->phase,LA_FAILED);}
        if(valid)valid=la_nv12_to_i420(c->pixels+(size_t)slot*LA_FRAME_BYTES,&source);
        valid=valid && c->ops.lease_valid(c->ops.context,&source) &&
              LOAD(&c->irq_seq)==seq && !la_cancelled(c);
        c->ops.lease_end(c->ops.context,&source);
        if(!valid){c->dropped++;return;}
        c->frames[slot]=(struct la_frame){stamp,seq/2};c->count++;c->copied++;
        if(!c->stream_start)c->stream_start=stamp;
        c->reason=LA_NONE;
        if(c->phase!=LA_POST && stamp-c->stream_start>=LA_WINDOW_US)
            STORE(&c->phase,LA_READY);
    }
finalize:
    if(c->phase==LA_POST && now>=c->press_us+LA_WINDOW_US) {
        uint64_t last=c->count?c->frames[(c->head+c->count-1)%LA_RING_CAPACITY].timestamp_us:0;
        if(!last || last<c->press_us+LA_WINDOW_US-LA_MAX_GAP_US) {
            c->reason=LA_GAP;STORE(&c->phase,LA_FAILED);
        } else STORE(&c->phase,LA_FROZEN);
    }
}
int la_begin_encode(struct la_capture *c) {
    if(c->phase!=LA_FROZEN || la_cancelled(c))return 0;
    STORE(&c->reader_active,1);STORE(&c->phase,LA_ENCODING);return 1;
}
const uint8_t *la_frame_at(const struct la_capture *c,uint32_t index,struct la_frame *meta) {
    if(c->phase!=LA_ENCODING || index>=c->count || la_cancelled(c))return 0;
    uint32_t slot=(c->head+index)%LA_RING_CAPACITY;
    if(meta)*meta=c->frames[slot];
    return c->pixels+(size_t)slot*LA_FRAME_BYTES;
}
void la_finish_encode(struct la_capture *c) {
    STORE(&c->reader_active,0);c->head=0;c->count=0;c->stream_start=0;c->press_us=0;
    if(la_cancelled(c)){reset(c);c->worker_epoch=LOAD(&c->cancel_epoch);}
    else STORE(&c->phase,LA_REQUESTED);
}
