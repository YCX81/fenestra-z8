#define SLOTS_ACTIVE 64u
/* slots.state: bounded access to the extended Picture Control records for the
 * card-state module. Z8 C3.11 emulation only. Addresses are fixed by the slots
 * layout; a module names records by index and can reach nothing else. */
#ifndef SLOTS_ACTIVE
#define SLOTS_ACTIVE 32u /* C32; the C64 build replaces it with 64u */
#endif
#define SLOTS_RECORD 1024u
#define SLOTS_RECORDS UINT64_C(0xa1166b080)
#define SLOTS_STAGE UINT64_C(0xa1168c000)
#define SLOTS_STATUS UINT64_C(0xa116d3000)
#define SLOTS_LIMIT_WORD UINT64_C(0xa116d3400) /* read by the virtual-key adapters; initial value SLOTS_ACTIVE */
#define SLOTS_SELECTORS UINT64_C(0xa02983cf8)
#define SLOTS_NATIVE_SELECT UINT64_C(0x403f96a8)
#define SLOTS_NATIVE_GET UINT64_C(0x410df224)
#define SLOTS_NATIVE_SET UINT64_C(0x41015d6c)
#define SLOTS_CACHE_INVALIDATE UINT64_C(0x40b03400)
#define SLOTS_INHERIT_KEY 0x0300122cu
#define SLOTS_MOVIE_CODE_KEY 0x0700122bu
enum {SLOTS_READ=1u,SLOTS_WRITE,SLOTS_COMMIT,SLOTS_SELECTION,SLOTS_APPLY,SLOTS_INVALIDATE,SLOTS_SUMMARY,SLOTS_HASH,SLOTS_STATUS_SET,SLOTS_INFO,SLOTS_LIMIT,SLOTS_PROXY};
/* Shutdown write-back words, next to the limit word: proxy photo, proxy movie, runs, last result. */
#define SLOTS_PROXY_WORDS (SLOTS_LIMIT_WORD+0x10u)
typedef struct {uint32_t index,offset,bytes,value;} SlotsRequest;
/* The core keeps no writable statics: stage ownership lives in the slots reservation.
 * Index 0 is never a valid extended slot, so zero means no stage. */
#define slots_stage_index (*(volatile uint32_t *)(uintptr_t)(SLOTS_LIMIT_WORD+8u))
#define slots_stage_bytes (*(volatile uint32_t *)(uintptr_t)(SLOTS_LIMIT_WORD+12u))
static uint32_t slots_limit(void){return *(volatile const uint32_t *)(uintptr_t)SLOTS_LIMIT_WORD;}
static int slots_index_ok(uint32_t i){uint32_t slot=i%64u;return i<128u && slot>=9u && slot<SLOTS_ACTIVE && slot<slots_limit();}
static uint8_t *slots_record(uint32_t i){return (uint8_t *)(uintptr_t)(SLOTS_RECORDS+(uint64_t)i*SLOTS_RECORD);}
static uint32_t slots_crc(uint32_t crc,const uint8_t *p,uint32_t n) {
    crc=~crc;
    while(n--){crc^=*p++;for(uint32_t k=0u;k<8u;++k)crc=crc&1u?(crc>>1)^UINT32_C(0xedb88320):crc>>1;}
    return ~crc;
}
static uint32_t slots_read32(const uint8_t *p){uint32_t v;memcpy(&v,p,4u);return v;}
static void slots_keys(uint32_t i,uint32_t keys[3]) {
    uint32_t bank=i/64u,slot=i%64u;
    for(uint32_t part=0u;part<3u;++part)
        keys[part]=(part==1u?0u:UINT32_C(0x01000000))|((0x7cu+bank)<<8)|slot<<2|part;
}
static int slots_photo_legal(uint32_t v){return v<0x28u || (v>=0x258u && v<=0x25au);}
static int slots_movie_legal(uint32_t v){return v>=50u && v<=89u;}
/* Event 7, after modules stopped and before the original handler saves settings:
 * never let an extended selection reach persistent settings. The card keeps the
 * real choice; the module restores it after the next mount. */
void fx_slots_shutdown(void) {
    volatile uint32_t *w=(volatile uint32_t *)(uintptr_t)SLOTS_PROXY_WORDS;
    const volatile uint32_t *sel=(const volatile uint32_t *)(uintptr_t)SLOTS_SELECTORS;
    uint32_t (*select)(uint32_t)=(uint32_t (*)(uint32_t))(uintptr_t)SLOTS_NATIVE_SELECT;
    uint32_t result=0u,photo=sel[0],movie=sel[1];
    if(photo>=0x1000u) {
        uint32_t known=w[0]>>31,target=w[0]&0xfffu;
        if(!known || !slots_photo_legal(target))target=0x258u;
        result|=select(target)==1u?1u:0x10u;
    }
    if(movie>=0x1000u) {
        uint32_t target=w[1]&0xfffu;
        if(w[1]>>31 && slots_movie_legal(target))result|=select(target)==1u?2u:0x20u;
        else {
            /* No movie fallback: follow the photo selection, which is legal by now, and put the
             * movie code back to what a new camera holds (0); following alone leaves it extended. */
            uint32_t scratch[4]={0u,0u,0u,0u};
            uint32_t (*set)(uint32_t,uint32_t,void *)=(uint32_t (*)(uint32_t,uint32_t,void *))(uintptr_t)SLOTS_NATIVE_SET;
            result|=set(SLOTS_INHERIT_KEY,1u,scratch)&1u?4u:0x40u;
            result|=set(SLOTS_MOVIE_CODE_KEY,0u,scratch)&1u?8u:0x80u;
        }
    }
    w[2]=w[2]+1u;w[3]=result;
}
static FxResult slots_state_call(const FxServiceRequest *q,FxServiceReply *a) {
    SlotsRequest r;
    if(q->bytes<sizeof(r))return FX_ARGUMENT;
    memcpy(&r,q->data,sizeof(r));
    switch(q->arg[2]) {
    case SLOTS_READ:
        if(!slots_index_ok(r.index) || !r.bytes || r.offset>SLOTS_RECORD || r.bytes>SLOTS_RECORD-r.offset || r.bytes>FX_SERVICE_BYTES)return FX_BOUNDS;
        memcpy(a->data,slots_record(r.index)+r.offset,r.bytes);a->bytes=r.bytes;return FX_OK;
    case SLOTS_WRITE:
        if(!slots_index_ok(r.index) || !r.bytes || r.offset>SLOTS_RECORD || r.bytes>SLOTS_RECORD-r.offset || q->bytes!=sizeof(r)+r.bytes)return FX_BOUNDS;
        /* The stage fills in order for one record; offset 0 starts it over. */
        if(!r.offset){slots_stage_index=r.index;slots_stage_bytes=0u;}
        if(r.index!=slots_stage_index || r.offset!=slots_stage_bytes)return FX_STATE;
        memcpy((uint8_t *)(uintptr_t)SLOTS_STAGE+r.offset,q->data+sizeof(r),r.bytes);slots_stage_bytes+=r.bytes;return FX_OK;
    case SLOTS_COMMIT: {
        /* A record replaces live state only when fully staged for this slot, with the flag and keys it must have. */
        if(!slots_index_ok(r.index))return FX_BOUNDS;
        if(r.index!=slots_stage_index || slots_stage_bytes!=SLOTS_RECORD)return FX_STATE;
        slots_stage_bytes=0u;
        const uint8_t *s=(const uint8_t *)(uintptr_t)SLOTS_STAGE;uint32_t keys[3];slots_keys(r.index,keys);
        if(slots_read32(s)!=1u)return FX_INTEGRITY;
        for(uint32_t part=0u;part<3u;++part)if(slots_read32(s+4u+4u*part)!=keys[part])return FX_INTEGRITY;
        memcpy(slots_record(r.index),s,SLOTS_RECORD);return FX_OK;
    }
    case SLOTS_SELECTION: {
        const volatile uint32_t *sel=(const volatile uint32_t *)(uintptr_t)SLOTS_SELECTORS;
        a->value[0]=sel[0];a->value[1]=sel[1];
        a->value[2]=((uint32_t (*)(uint32_t))(uintptr_t)SLOTS_NATIVE_GET)(SLOTS_INHERIT_KEY);
        return FX_OK;
    }
    case SLOTS_APPLY: {
        uint32_t base=r.value&0xf000u,index=(r.value&0xfffu)+(base==0x2000u?64u:0u);
        if(r.index>1u || (r.index?base!=0x2000u:base!=0x1000u) || !slots_index_ok(index) || slots_read32(slots_record(index))!=1u)return FX_BOUNDS;
        if(r.index) {
            uint32_t scratch[4]={0u,0u,0u,0u};
            if(!(((uint32_t (*)(uint32_t,uint32_t,void *))(uintptr_t)SLOTS_NATIVE_SET)(SLOTS_INHERIT_KEY,0u,scratch)&1u))return FX_IO;
        }
        a->value[0]=((uint32_t (*)(uint32_t))(uintptr_t)SLOTS_NATIVE_SELECT)(r.value);
        return a->value[0]==1u?FX_OK:FX_IO;
    }
    case SLOTS_INVALIDATE:
        ((void (*)(void))(uintptr_t)SLOTS_CACHE_INVALIDATE)();return FX_OK;
    case SLOTS_SUMMARY: {
        uint32_t crc=0u;
        for(uint32_t i=0u;i<128u;++i)if(slots_index_ok(i))crc=slots_crc(crc,slots_record(i),SLOTS_RECORD);
        a->value[0]=crc;return FX_OK;
    }
    case SLOTS_HASH:
        if(!slots_index_ok(r.index))return FX_BOUNDS;
        a->value[0]=slots_crc(0u,slots_record(r.index),SLOTS_RECORD);
        a->value[1]=slots_read32(slots_record(r.index));return FX_OK;
    case SLOTS_STATUS_SET:
        if(r.bytes>256u || q->bytes!=sizeof(r)+r.bytes)return FX_BOUNDS;
        memcpy((uint8_t *)(uintptr_t)SLOTS_STATUS,q->data+sizeof(r),r.bytes);return FX_OK;
    case SLOTS_PROXY: {
        /* Stock-legal fallbacks the shutdown write-back will select; 0 = none known. */
        volatile uint32_t *w=(volatile uint32_t *)(uintptr_t)SLOTS_PROXY_WORDS;
        if(r.value>=0x1000u || r.index>=0x1000u)return FX_BOUNDS;
        /* Bit 31 marks a known value: selector 0 is a legal photo choice, not "unknown". */
        w[0]=r.value|UINT32_C(0x80000000);w[1]=r.index|UINT32_C(0x80000000);return FX_OK;
    }
    case SLOTS_INFO:
        a->value[0]=SLOTS_ACTIVE;a->value[1]=slots_limit();return FX_OK;
    case SLOTS_LIMIT:
        /* Only the two designed limits, never above what this core was built for. */
        if((r.value!=32u && r.value!=64u) || r.value>SLOTS_ACTIVE)return FX_BOUNDS;
        *(volatile uint32_t *)(uintptr_t)SLOTS_LIMIT_WORD=r.value;
        ((void (*)(void))(uintptr_t)SLOTS_CACHE_INVALIDATE)();return FX_OK;
    default:return FX_ARGUMENT;
    }
}
