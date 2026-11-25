/* See slotcard.h. On-card form, one immutable file per generation:
 *   header 592 B, N entries {u16 slot,u16 0,u32 crc,1024 B}, footer 24 B.
 * A generation counts only when header, footer and whole-file CRC all check. Each
 * header holds the full slot->generation table, so a bad old file loses only the
 * slots whose latest copy lived there. Files are never modified after creation. */
#include <stddef.h>
#include "slotcard.h"
#define MAGIC "Z8SCARD1"
#define FOOT UINT32_C(0x4b4f4353)
_Static_assert((offsetof(Sc, records) & 7u) == 0u, "Sc.records must be 8B aligned");
_Static_assert((offsetof(Sc, file) & 7u) == 0u, "Sc.file must be 8B aligned");
#if defined(__GNUC__) || defined(__clang__)
typedef uint32_t ScWord32 __attribute__((__may_alias__));
typedef uint64_t ScWord64 __attribute__((__may_alias__));
#endif
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put64(uint8_t *p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static uint64_t get64(const uint8_t *p){return (uint64_t)get32(p)|(uint64_t)get32(p+4)<<32;}
static void bytes_copy(uint8_t *d,const uint8_t *s,uint32_t n) {
#if defined(__GNUC__) || defined(__clang__)
    if((((uintptr_t)d^(uintptr_t)s)&7u)==0u) {
        while(n && ((uintptr_t)d&7u)){*d++=*s++;--n;}
        if(n>=8u) {
            ScWord64 *dw=(ScWord64 *)(void *)d;
            const ScWord64 *sw=(const ScWord64 *)(const void *)s;
            while(n>=32u){dw[0]=sw[0];dw[1]=sw[1];dw[2]=sw[2];dw[3]=sw[3];dw+=4;sw+=4;n-=32u;}
            while(n>=8u){*dw++=*sw++;n-=8u;}
            d=(uint8_t *)dw;s=(const uint8_t *)sw;
        }
    }
#endif
    while(n--){*d++=*s++;}
}
static void bytes_zero(uint8_t *d,uint32_t n) {
#if defined(__GNUC__) || defined(__clang__)
    while(n && ((uintptr_t)d&7u)){*d++=0u;--n;}
    if(n>=8u) {
        ScWord64 *dw=(ScWord64 *)(void *)d;
        while(n>=32u){dw[0]=0u;dw[1]=0u;dw[2]=0u;dw[3]=0u;dw+=4;n-=32u;}
        while(n>=8u){*dw++=0u;n-=8u;}
        d=(uint8_t *)dw;
    }
#endif
    while(n--){*d++=0u;}
}
/* IEEE 802.3 CRC32 (reflected polynomial 0xedb88320), 1 KiB .rodata, 0 relocations. */
static const uint32_t crc_tab[256] = {
    UINT32_C(0x00000000), UINT32_C(0x77073096), UINT32_C(0xee0e612c), UINT32_C(0x990951ba),
    UINT32_C(0x076dc419), UINT32_C(0x706af48f), UINT32_C(0xe963a535), UINT32_C(0x9e6495a3),
    UINT32_C(0x0edb8832), UINT32_C(0x79dcb8a4), UINT32_C(0xe0d5e91e), UINT32_C(0x97d2d988),
    UINT32_C(0x09b64c2b), UINT32_C(0x7eb17cbd), UINT32_C(0xe7b82d07), UINT32_C(0x90bf1d91),
    UINT32_C(0x1db71064), UINT32_C(0x6ab020f2), UINT32_C(0xf3b97148), UINT32_C(0x84be41de),
    UINT32_C(0x1adad47d), UINT32_C(0x6ddde4eb), UINT32_C(0xf4d4b551), UINT32_C(0x83d385c7),
    UINT32_C(0x136c9856), UINT32_C(0x646ba8c0), UINT32_C(0xfd62f97a), UINT32_C(0x8a65c9ec),
    UINT32_C(0x14015c4f), UINT32_C(0x63066cd9), UINT32_C(0xfa0f3d63), UINT32_C(0x8d080df5),
    UINT32_C(0x3b6e20c8), UINT32_C(0x4c69105e), UINT32_C(0xd56041e4), UINT32_C(0xa2677172),
    UINT32_C(0x3c03e4d1), UINT32_C(0x4b04d447), UINT32_C(0xd20d85fd), UINT32_C(0xa50ab56b),
    UINT32_C(0x35b5a8fa), UINT32_C(0x42b2986c), UINT32_C(0xdbbbc9d6), UINT32_C(0xacbcf940),
    UINT32_C(0x32d86ce3), UINT32_C(0x45df5c75), UINT32_C(0xdcd60dcf), UINT32_C(0xabd13d59),
    UINT32_C(0x26d930ac), UINT32_C(0x51de003a), UINT32_C(0xc8d75180), UINT32_C(0xbfd06116),
    UINT32_C(0x21b4f4b5), UINT32_C(0x56b3c423), UINT32_C(0xcfba9599), UINT32_C(0xb8bda50f),
    UINT32_C(0x2802b89e), UINT32_C(0x5f058808), UINT32_C(0xc60cd9b2), UINT32_C(0xb10be924),
    UINT32_C(0x2f6f7c87), UINT32_C(0x58684c11), UINT32_C(0xc1611dab), UINT32_C(0xb6662d3d),
    UINT32_C(0x76dc4190), UINT32_C(0x01db7106), UINT32_C(0x98d220bc), UINT32_C(0xefd5102a),
    UINT32_C(0x71b18589), UINT32_C(0x06b6b51f), UINT32_C(0x9fbfe4a5), UINT32_C(0xe8b8d433),
    UINT32_C(0x7807c9a2), UINT32_C(0x0f00f934), UINT32_C(0x9609a88e), UINT32_C(0xe10e9818),
    UINT32_C(0x7f6a0dbb), UINT32_C(0x086d3d2d), UINT32_C(0x91646c97), UINT32_C(0xe6635c01),
    UINT32_C(0x6b6b51f4), UINT32_C(0x1c6c6162), UINT32_C(0x856530d8), UINT32_C(0xf262004e),
    UINT32_C(0x6c0695ed), UINT32_C(0x1b01a57b), UINT32_C(0x8208f4c1), UINT32_C(0xf50fc457),
    UINT32_C(0x65b0d9c6), UINT32_C(0x12b7e950), UINT32_C(0x8bbeb8ea), UINT32_C(0xfcb9887c),
    UINT32_C(0x62dd1ddf), UINT32_C(0x15da2d49), UINT32_C(0x8cd37cf3), UINT32_C(0xfbd44c65),
    UINT32_C(0x4db26158), UINT32_C(0x3ab551ce), UINT32_C(0xa3bc0074), UINT32_C(0xd4bb30e2),
    UINT32_C(0x4adfa541), UINT32_C(0x3dd895d7), UINT32_C(0xa4d1c46d), UINT32_C(0xd3d6f4fb),
    UINT32_C(0x4369e96a), UINT32_C(0x346ed9fc), UINT32_C(0xad678846), UINT32_C(0xda60b8d0),
    UINT32_C(0x44042d73), UINT32_C(0x33031de5), UINT32_C(0xaa0a4c5f), UINT32_C(0xdd0d7cc9),
    UINT32_C(0x5005713c), UINT32_C(0x270241aa), UINT32_C(0xbe0b1010), UINT32_C(0xc90c2086),
    UINT32_C(0x5768b525), UINT32_C(0x206f85b3), UINT32_C(0xb966d409), UINT32_C(0xce61e49f),
    UINT32_C(0x5edef90e), UINT32_C(0x29d9c998), UINT32_C(0xb0d09822), UINT32_C(0xc7d7a8b4),
    UINT32_C(0x59b33d17), UINT32_C(0x2eb40d81), UINT32_C(0xb7bd5c3b), UINT32_C(0xc0ba6cad),
    UINT32_C(0xedb88320), UINT32_C(0x9abfb3b6), UINT32_C(0x03b6e20c), UINT32_C(0x74b1d29a),
    UINT32_C(0xead54739), UINT32_C(0x9dd277af), UINT32_C(0x04db2615), UINT32_C(0x73dc1683),
    UINT32_C(0xe3630b12), UINT32_C(0x94643b84), UINT32_C(0x0d6d6a3e), UINT32_C(0x7a6a5aa8),
    UINT32_C(0xe40ecf0b), UINT32_C(0x9309ff9d), UINT32_C(0x0a00ae27), UINT32_C(0x7d079eb1),
    UINT32_C(0xf00f9344), UINT32_C(0x8708a3d2), UINT32_C(0x1e01f268), UINT32_C(0x6906c2fe),
    UINT32_C(0xf762575d), UINT32_C(0x806567cb), UINT32_C(0x196c3671), UINT32_C(0x6e6b06e7),
    UINT32_C(0xfed41b76), UINT32_C(0x89d32be0), UINT32_C(0x10da7a5a), UINT32_C(0x67dd4acc),
    UINT32_C(0xf9b9df6f), UINT32_C(0x8ebeeff9), UINT32_C(0x17b7be43), UINT32_C(0x60b08ed5),
    UINT32_C(0xd6d6a3e8), UINT32_C(0xa1d1937e), UINT32_C(0x38d8c2c4), UINT32_C(0x4fdff252),
    UINT32_C(0xd1bb67f1), UINT32_C(0xa6bc5767), UINT32_C(0x3fb506dd), UINT32_C(0x48b2364b),
    UINT32_C(0xd80d2bda), UINT32_C(0xaf0a1b4c), UINT32_C(0x36034af6), UINT32_C(0x41047a60),
    UINT32_C(0xdf60efc3), UINT32_C(0xa867df55), UINT32_C(0x316e8eef), UINT32_C(0x4669be79),
    UINT32_C(0xcb61b38c), UINT32_C(0xbc66831a), UINT32_C(0x256fd2a0), UINT32_C(0x5268e236),
    UINT32_C(0xcc0c7795), UINT32_C(0xbb0b4703), UINT32_C(0x220216b9), UINT32_C(0x5505262f),
    UINT32_C(0xc5ba3bbe), UINT32_C(0xb2bd0b28), UINT32_C(0x2bb45a92), UINT32_C(0x5cb36a04),
    UINT32_C(0xc2d7ffa7), UINT32_C(0xb5d0cf31), UINT32_C(0x2cd99e8b), UINT32_C(0x5bdeae1d),
    UINT32_C(0x9b64c2b0), UINT32_C(0xec63f226), UINT32_C(0x756aa39c), UINT32_C(0x026d930a),
    UINT32_C(0x9c0906a9), UINT32_C(0xeb0e363f), UINT32_C(0x72076785), UINT32_C(0x05005713),
    UINT32_C(0x95bf4a82), UINT32_C(0xe2b87a14), UINT32_C(0x7bb12bae), UINT32_C(0x0cb61b38),
    UINT32_C(0x92d28e9b), UINT32_C(0xe5d5be0d), UINT32_C(0x7cdcefb7), UINT32_C(0x0bdbdf21),
    UINT32_C(0x86d3d2d4), UINT32_C(0xf1d4e242), UINT32_C(0x68ddb3f8), UINT32_C(0x1fda836e),
    UINT32_C(0x81be16cd), UINT32_C(0xf6b9265b), UINT32_C(0x6fb077e1), UINT32_C(0x18b74777),
    UINT32_C(0x88085ae6), UINT32_C(0xff0f6a70), UINT32_C(0x66063bca), UINT32_C(0x11010b5c),
    UINT32_C(0x8f659eff), UINT32_C(0xf862ae69), UINT32_C(0x616bffd3), UINT32_C(0x166ccf45),
    UINT32_C(0xa00ae278), UINT32_C(0xd70dd2ee), UINT32_C(0x4e048354), UINT32_C(0x3903b3c2),
    UINT32_C(0xa7672661), UINT32_C(0xd06016f7), UINT32_C(0x4969474d), UINT32_C(0x3e6e77db),
    UINT32_C(0xaed16a4a), UINT32_C(0xd9d65adc), UINT32_C(0x40df0b66), UINT32_C(0x37d83bf0),
    UINT32_C(0xa9bcae53), UINT32_C(0xdebb9ec5), UINT32_C(0x47b2cf7f), UINT32_C(0x30b5ffe9),
    UINT32_C(0xbdbdf21c), UINT32_C(0xcabac28a), UINT32_C(0x53b39330), UINT32_C(0x24b4a3a6),
    UINT32_C(0xbad03605), UINT32_C(0xcdd70693), UINT32_C(0x54de5729), UINT32_C(0x23d967bf),
    UINT32_C(0xb3667a2e), UINT32_C(0xc4614ab8), UINT32_C(0x5d681b02), UINT32_C(0x2a6f2b94),
    UINT32_C(0xb40bbe37), UINT32_C(0xc30c8ea1), UINT32_C(0x5a05df1b), UINT32_C(0x2d02ef8d)
};
uint32_t sc_crc32(uint32_t crc,const void *data,uint32_t n) {
    const uint8_t *p=(const uint8_t *)data;
    crc=~crc;
#if (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    while(n && ((uintptr_t)p&3u)){crc=(crc>>8)^crc_tab[(crc^*p++)&0xffu];--n;}
    if(n>=4u) {
        const ScWord32 *w=(const ScWord32 *)(const void *)p;
        while(n>=4u) {
            crc^=*w++;
            crc=(crc>>8)^crc_tab[crc&0xffu];
            crc=(crc>>8)^crc_tab[crc&0xffu];
            crc=(crc>>8)^crc_tab[crc&0xffu];
            crc=(crc>>8)^crc_tab[crc&0xffu];
            n-=4u;
        }
        p=(const uint8_t *)w;
    }
#endif
    while(n--){crc=(crc>>8)^crc_tab[(crc^*p++)&0xffu];}
    return ~crc;
}
void sc_name(uint32_t gen,char out[16]) {
    static const char hex[]="0123456789abcdef";
    out[0]='s';out[1]='c';
    for(uint32_t i=0u;i<8u;++i)out[2u+i]=hex[(gen>>(28u-4u*i))&15u];
    out[10]='.';out[11]='r';out[12]='e';out[13]='c';out[14]=0;out[15]=0;
}
void sc_init(Sc *s,uint64_t entropy,uint32_t use_scan) {
    bytes_zero((uint8_t *)s,(uint32_t)sizeof(*s));
    s->entropy=entropy;s->use_scan=use_scan;s->next_gen=1u;
}
void sc_put_record(Sc *s,uint32_t slot,const uint8_t *data) {
    if(slot<SC_SLOTS){bytes_copy(s->records[slot],data,SC_RECORD);s->dirty[slot]=1u;}
}
static void forget(Sc *s) {
    s->store_id=0u;s->head=0u;s->next_gen=1u;
    for(uint32_t i=0u;i<SC_SLOTS;++i){s->table[i]=0u;s->used[i]=0u;s->bad[i]=0u;s->dirty[i]=0u;}
    for(uint32_t b=0u;b<2u;++b){s->selection[b]=0u;s->proxy[b]=0u;s->next_selection[b]=0u;s->next_proxy[b]=0u;}
    s->limit=0u;s->next_limit=0u;
    s->files_skipped=0u;s->bad_slots=0u;
}
enum {M_NONE,M_LOAD,M_COMMIT,M_CHECK,M_GC};
void sc_begin_load(Sc *s){forget(s);s->mode=M_LOAD;s->pc=0u;s->pc2=0u;s->status=SC_OK;}
void sc_begin_commit(Sc *s){s->mode=M_COMMIT;s->pc=0u;s->pc2=0u;s->status=SC_OK;}
void sc_begin_check(Sc *s){s->mode=M_CHECK;s->pc=0u;s->pc2=0u;s->status=SC_OK;}
void sc_begin_gc(Sc *s){s->mode=M_GC;s->pc=0u;s->pc2=0u;s->status=SC_OK;}

static int header_ok(const uint8_t *h,uint32_t gen) {
    for(uint32_t i=0u;i<8u;++i)if(h[i]!=(uint8_t)MAGIC[i])return 0;
    if(get32(h+8)!=1u || get32(h+12)!=SC_HEADER || get32(h+16)!=gen || !get64(h+24))return 0;
    if(get32(h+36)>SC_FILE_RECORDS)return 0;
    return sc_crc32(0u,h,SC_HEADER-4u)==get32(h+SC_HEADER-4u);
}
static uint32_t file_size(const uint8_t *h){return SC_HEADER+get32(h+36)*SC_ENTRY+SC_FOOTER;}
static int footer_ok(const uint8_t *f,uint32_t size,uint32_t gen,uint32_t file_crc) {
    const uint8_t *t=f+size-SC_FOOTER;
    return get32(t)==FOOT && get32(t+4)==size && get32(t+8)==gen &&
        get64(t+12)==get64(f+24) && get32(t+20)==file_crc;
}

/* Sub-machine: open gen, read it into s->file, leave v_ok. v_ok=2 means the file
 * was absent, 3 means the card did not answer. */
#define Y2 do{s->pc2=__LINE__;return SC_RUN;case __LINE__:;}while(0)
static uint32_t validate(Sc *s,ScIo *io) {
    switch(s->pc2) {
    case 0u:
        s->v_ok=0u;
        *io=(ScIo){SC_IO_OPEN_READ,s->v_gen,0u,0,0};Y2;
        if(s->reply.result==SC_R_MISSING){s->v_ok=2u;s->pc2=0u;return SC_DONE;}
        if(s->reply.result!=SC_R_OK){s->v_ok=3u;s->pc2=0u;return SC_DONE;}
        s->offset=0u;s->total=SC_HEADER;s->v_crc=0u;
        while(s->offset<s->total) {
            s->count=s->total-s->offset;if(s->count>SC_WIRE)s->count=SC_WIRE;
            *io=(ScIo){SC_IO_READ,0u,s->count,s->file+s->offset,0};Y2;
            if(s->reply.result!=SC_R_OK || !s->reply.done || s->reply.done>s->count){s->total=0u;break;}
            {
                uint32_t prev=s->offset,got=s->reply.done;
                s->offset+=got;
                if(s->total==SC_HEADER) {
                    /* All header bytes [0,592) strictly precede footer CRC [total-4,total) (total>=616). */
                    s->v_crc=sc_crc32(s->v_crc,s->file+prev,got);
                    if(s->offset==SC_HEADER) {
                        if(!header_ok(s->file,s->v_gen)){s->total=0u;break;}
                        s->total=file_size(s->file);
                    }
                } else {
                    uint32_t crc_end=s->total-4u;
                    if(prev<crc_end) {
                        uint32_t n=s->offset<crc_end?got:crc_end-prev;
                        s->v_crc=sc_crc32(s->v_crc,s->file+prev,n);
                    }
                }
            }
        }
        /* A short read still lets the caller salvage records from s->offset bytes. */
        if(s->total && s->offset==s->total && footer_ok(s->file,s->total,s->v_gen,s->v_crc))s->v_ok=1u;
        *io=(ScIo){SC_IO_CLOSE,0u,0u,0,0};Y2;
        if(s->reply.result!=SC_R_OK)s->v_ok=s->v_ok==1u?1u:0u;
        s->pc2=0u;return SC_DONE;
    default:break;
    }
    return SC_DONE;
}
#define Y do{s->pc=__LINE__;return SC_RUN;case __LINE__:;}while(0)
#define CHECK_FILE(g) do{s->v_gen=(g);s->pc2=0u;while(validate(s,io)==SC_RUN){Y;}}while(0)
#define FINISH(st) do{s->status=(st);s->pc=0u;s->mode=M_NONE;return SC_DONE;}while(0)

static void build_file(Sc *s,uint32_t gen,uint32_t *out_size) {
    uint8_t *f=s->file;bytes_zero(f,SC_HEADER);
    for(uint32_t i=0u;i<8u;++i)f[i]=(uint8_t)MAGIC[i];
    put32(f+8,1u);put32(f+12,SC_HEADER);put32(f+16,gen);put64(f+24,s->store_id);
    uint32_t at=SC_HEADER,n=0u;
    for(uint32_t i=0u;i<SC_SLOTS && n<SC_FILE_RECORDS;++i)if(s->dirty[i]) {
        f[at]=(uint8_t)i;f[at+1u]=0u;f[at+2u]=0u;f[at+3u]=0u;
        put32(f+at+4u,sc_crc32(0u,s->records[i],SC_RECORD));
        bytes_copy(f+at+8u,s->records[i],SC_RECORD);at+=SC_ENTRY;++n;
    }
    put32(f+36,n);put32(f+40,s->next_selection[0]);put32(f+44,s->next_selection[1]);
    put32(f+48,s->next_proxy[0]);put32(f+52,s->next_proxy[1]);put32(f+56,s->next_limit);
    for(uint32_t i=0u;i<SC_SLOTS;++i) {
        uint32_t g=s->table[i];
        /* In-file dirty entries refer to this generation. */
        if(s->dirty[i]) {
            for(uint32_t e=0u;e<n;++e)if(f[SC_HEADER+e*SC_ENTRY]==i){g=gen;break;}
        }
        put32(f+64u+4u*i,g);
    }
    put32(f+SC_HEADER-4u,sc_crc32(0u,f,SC_HEADER-4u));
    put32(f+at,FOOT);put32(f+at+4u,at+SC_FOOTER);put32(f+at+8u,gen);put64(f+at+12u,s->store_id);
    put32(f+at+20u,sc_crc32(0u,f,at+20u));
    *out_size=at+SC_FOOTER;
}

static uint32_t load_step(Sc *s,ScIo *io) {
    switch(s->pc) {
    case 0u:
        if(s->use_scan) {
            *io=(ScIo){SC_IO_SCAN,0u,0u,0,0};Y;
            if(s->reply.result==SC_R_MISSING){s->a=0u;goto found;}
            if(s->reply.result!=SC_R_OK)FINISH(SC_NO_MEDIA);
            s->a=s->reply.value;goto found;
        }
        /* Existence is monotonic without a delete service: gallop, then bisect. */
        s->a=0u;s->b=1u;
        for(;;) {
            *io=(ScIo){SC_IO_OPEN_READ,s->b,0u,0,0};Y;
            if(s->reply.result==SC_R_MISSING)break;
            if(s->reply.result!=SC_R_OK)FINISH(SC_NO_MEDIA);
            *io=(ScIo){SC_IO_CLOSE,0u,0u,0,0};Y;
            s->a=s->b;
            if(s->b>SC_MAX_GEN/2u)FINISH(SC_BAD_ARGUMENT);
            s->b*=2u;
        }
        while(s->b-s->a>1u) {
            s->c=s->a+(s->b-s->a)/2u;
            *io=(ScIo){SC_IO_OPEN_READ,s->c,0u,0,0};Y;
            if(s->reply.result==SC_R_MISSING){s->b=s->c;continue;}
            if(s->reply.result!=SC_R_OK)FINISH(SC_NO_MEDIA);
            *io=(ScIo){SC_IO_CLOSE,0u,0u,0,0};Y;
            s->a=s->c;
        }
    found:
        s->next_gen=s->a+1u;
        if(!s->a)FINISH(SC_EMPTY_STORE);
        for(s->cand=s->a,s->tries=0u;s->cand && s->tries<SC_WALKBACK;--s->cand,++s->tries) {
            CHECK_FILE(s->cand);
            if(s->v_ok==3u)FINISH(SC_NO_MEDIA);
            if(s->v_ok==1u)break;
            ++s->files_skipped;
        }
        if(!s->cand || s->tries==SC_WALKBACK)FINISH(SC_CORRUPT_ALL);
        /* Header in s->file names the live state. */
        s->head=s->cand;s->store_id=get64(s->file+24);
        s->selection[0]=get32(s->file+40);s->selection[1]=get32(s->file+44);
        s->proxy[0]=get32(s->file+48);s->proxy[1]=get32(s->file+52);s->limit=get32(s->file+56);
        for(uint32_t i=0u;i<SC_SLOTS;++i){s->table[i]=get32(s->file+64u+4u*i);if(s->table[i]>s->head)s->table[i]=0u;}
        /* Fetch each referenced generation once, newest first. */
        for(;;) {
            s->d=0u;
            for(uint32_t i=0u;i<SC_SLOTS;++i)
                if(s->table[i] && !s->used[i] && !s->bad[i] && s->table[i]>s->d)s->d=s->table[i];
            if(!s->d)break;
            CHECK_FILE(s->d);
            if(s->v_ok==3u)FINISH(SC_NO_MEDIA);
            {
                /* Trust per-entry CRCs even when the whole file fails, but only with
                 * a good header and only for what was actually read. */
                uint32_t entries=s->v_ok==1u?get32(s->file+36)
                    :(s->v_ok==2u || !header_ok(s->file,s->d)?0u:get32(s->file+36));
                if(s->total)for(uint32_t e=0u;e<entries;++e) {
                    uint32_t at=SC_HEADER+e*SC_ENTRY,slot=s->file[at];
                    if(at+SC_ENTRY>s->offset || slot>=SC_SLOTS || s->table[slot]!=s->d)continue;
                    if(sc_crc32(0u,s->file+at+8u,SC_RECORD)==get32(s->file+at+4u)) {
                        bytes_copy(s->records[slot],s->file+at+8u,SC_RECORD);s->used[slot]=1u;
                    }
                }
                for(uint32_t i=0u;i<SC_SLOTS;++i)if(s->table[i]==s->d && !s->used[i]){s->bad[i]=1u;++s->bad_slots;}
            }
        }
        FINISH(SC_OK);
    default:break;
    }
    return SC_DONE;
}
static uint32_t commit_step(Sc *s,ScIo *io) {
    switch(s->pc) {
    case 0u:
        if(!s->store_id)s->store_id=s->entropy?s->entropy:1u;
    next_file:
        s->tries=0u;
    retry_create:
        build_file(s,s->next_gen,&s->total);
        *io=(ScIo){SC_IO_CREATE,s->next_gen,0u,0,0};Y;
        if(s->reply.result!=SC_R_OK) {
            /* Collision and no-media look alike: ask whether the name exists. */
            *io=(ScIo){SC_IO_OPEN_READ,s->next_gen,0u,0,0};Y;
            if(s->reply.result==SC_R_OK) {
                *io=(ScIo){SC_IO_CLOSE,0u,0u,0,0};Y;
                ++s->next_gen;
                if(++s->tries<16u)goto retry_create;
                FINISH(SC_WRITE_FAILED);
            }
            FINISH(s->reply.result==SC_R_MISSING?SC_WRITE_FAILED:SC_NO_MEDIA);
        }
        s->a=s->next_gen;s->offset=0u;
        while(s->offset<s->total) {
            s->count=s->total-s->offset;if(s->count>SC_WIRE)s->count=SC_WIRE;
            *io=(ScIo){SC_IO_WRITE,0u,s->count,0,s->file+s->offset};Y;
            if(s->reply.result!=SC_R_OK || s->reply.done!=s->count) {
                *io=(ScIo){SC_IO_CLOSE,0u,0u,0,0};Y;
                ++s->next_gen;++s->failed_gens;
                FINISH(SC_WRITE_FAILED);
            }
            s->offset+=s->count;
        }
        *io=(ScIo){SC_IO_CLOSE,0u,0u,0,0};Y;
        if(s->reply.result!=SC_R_OK){++s->next_gen;++s->failed_gens;FINISH(SC_WRITE_FAILED);}
        /* The in-memory table may change only once the card reads back correct. */
        CHECK_FILE(s->a);
        if(s->v_ok!=1u){++s->next_gen;++s->failed_gens;FINISH(s->v_ok==3u?SC_NO_MEDIA:SC_VERIFY_FAILED);}
        {
            uint32_t n=get32(s->file+36);
            for(uint32_t e=0u;e<n;++e) {
                uint32_t slot=s->file[SC_HEADER+e*SC_ENTRY];
                s->table[slot]=s->a;s->used[slot]=1u;s->bad[slot]=0u;s->dirty[slot]=0u;
            }
        }
        s->selection[0]=s->next_selection[0];s->selection[1]=s->next_selection[1];
        s->proxy[0]=s->next_proxy[0];s->proxy[1]=s->next_proxy[1];s->limit=s->next_limit;
        s->head=s->a;s->last_gen_written=s->a;s->next_gen=s->a+1u;
        for(uint32_t i=0u;i<SC_SLOTS;++i)if(s->dirty[i])goto next_file;
        FINISH(SC_OK);
    default:break;
    }
    return SC_DONE;
}
static uint32_t check_step(Sc *s,ScIo *io) {
    switch(s->pc) {
    case 0u:
        /* Empty view: a card that has generation 1 is a different store. */
        s->a=s->head?s->head:1u;
        *io=(ScIo){SC_IO_OPEN_READ,s->a,0u,0,0};Y;
        if(s->reply.result==SC_R_MISSING)FINISH(s->head?SC_OTHER_CARD:SC_SAME_CARD);
        if(s->reply.result!=SC_R_OK)FINISH(SC_NO_MEDIA);
        *io=(ScIo){SC_IO_READ,0u,SC_HEADER,s->file,0};Y;
        s->b=s->reply.result==SC_R_OK && s->reply.done==SC_HEADER && header_ok(s->file,s->a);
        *io=(ScIo){SC_IO_CLOSE,0u,0u,0,0};Y;
        if(!s->head)FINISH(SC_OTHER_CARD);
        if(!s->b)FINISH(SC_OTHER_CARD);
        FINISH(get64(s->file+24)==s->store_id?SC_SAME_CARD:SC_OTHER_CARD);
    default:break;
    }
    return SC_DONE;
}
static uint32_t gc_step(Sc *s,ScIo *io) {
    switch(s->pc) {
    case 0u:
        /* Keep the head, its predecessor (fallback) and everything the table names. */
        if(!s->use_scan || s->head<3u)FINISH(SC_OK);
        for(s->a=s->gc_floor?s->gc_floor:1u;s->a+1u<s->head;++s->a) {
            s->b=0u;
            for(uint32_t i=0u;i<SC_SLOTS;++i)if(s->table[i]==s->a)s->b=1u;
            if(s->b)continue;
            *io=(ScIo){SC_IO_DELETE,s->a,0u,0,0};Y;
            if(s->reply.result==SC_R_FAILED)FINISH(SC_NO_MEDIA);
        }
        s->gc_floor=s->a;
        FINISH(SC_OK);
    default:break;
    }
    return SC_DONE;
}
uint32_t sc_next(Sc *s,const ScReply *r,ScIo *io) {
    if(s->pc || s->pc2){s->reply=*r;++s->io_count;}
    switch(s->mode) {
    case M_LOAD:return load_step(s,io);
    case M_COMMIT:return commit_step(s,io);
    case M_CHECK:return check_step(s,io);
    case M_GC:return gc_step(s,io);
    default:return SC_DONE;
    }
}

/* Selector encoding matches Layout.selector in the existing slots tree (facts only). */
int sc_selector_extended(uint32_t v,uint32_t *index) {
    uint32_t bank=v>>12;
    if(bank!=1u && bank!=2u)return 0;
    uint32_t slot=v&0xfffu;
    if(slot<9u || slot>=64u)return 0;
    if(index)*index=(bank-1u)*64u+slot;
    return 1;
}
uint32_t sc_selector_of(uint32_t index) {return UINT32_C(0x1000)*(index/64u+1u)+index%64u;}
int sc_selector_legal(uint32_t bank,uint32_t v) {
    /* 0x410a7bcc accepts <0x28 or 0x258..0x25a for photo; 0x410a7d84's movie range is
     * narrowed here to what stock's own C1..C9 and built-ins use. Reviewed statically. */
    if(bank==0u)return v<0x28u || (v>=0x258u && v<=0x25au);
    return (v>=50u && v<=89u) || v==0u;
}
uint32_t sc_apply_after_mount(const Sc *s,const ScNative *n) {
    uint32_t applied=0u;
    for(uint32_t b=0u;b<2u;++b) {
        uint32_t index,want=s->selection[b];
        if(!sc_selector_extended(want,&index) || !s->used[index] || s->bad[index])continue;
        /* The window produced a different value: the user chose it, keep theirs. */
        if(n->get(n->context,b)!=s->proxy[b])continue;
        if(n->set(n->context,b,want))++applied;
    }
    return applied;
}
void sc_note_selection(Sc *s,uint32_t bank,uint32_t v) {
    if(bank>1u)return;
    s->next_selection[bank]=v;
    if(sc_selector_legal(bank,v))s->next_proxy[bank]=v;
    else if(!s->next_proxy[bank])s->next_proxy[bank]=bank?50u:1u;
    for(uint32_t b=0u;b<2u;++b) {
        if(b==bank)continue;
        if(!s->next_selection[b] && s->selection[b])s->next_selection[b]=s->selection[b];
        if(!s->next_proxy[b])s->next_proxy[b]=s->proxy[b]?s->proxy[b]:(b?50u:1u);
    }
}
uint32_t sc_card_changed(Sc *s,const ScNative *n) {
    uint32_t dropped=0u;
    for(uint32_t i=0u;i<SC_SLOTS;++i)dropped+=s->used[i];
    for(uint32_t b=0u;b<2u;++b)if(sc_selector_extended(n->get(n->context,b),0))
        (void)n->set(n->context,b,s->proxy[b]?s->proxy[b]:(b?50u:1u));
    uint64_t entropy=s->entropy;uint32_t scan=s->use_scan;
    forget(s);s->entropy=entropy;s->use_scan=scan;s->gc_floor=0u;
    return dropped;
}
int sc_restore_for_shutdown(const Sc *s,const ScNative *n) {
    int rewritten=0;
    for(uint32_t b=0u;b<2u;++b) {
        uint32_t now=n->get(n->context,b);
        if(sc_selector_legal(b,now))continue;
        uint32_t target=s->proxy[b];
        if(!sc_selector_legal(b,target) || !target)target=b?50u:1u;
        if(!n->set(n->context,b,target) || n->get(n->context,b)!=target)return -1;
        ++rewritten;
    }
    return rewritten;
}
