#ifndef LIVE_CONFIG_H
#define LIVE_CONFIG_H
#include <stdint.h>
#define LIVE_CONFIG_SIZE 64u
static inline uint32_t live_config_crc(const uint8_t *p, uint32_t n) {
    uint32_t c=~0u;
    while(n--) {c^=*p++;for(uint32_t b=0;b<8u;b++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}
    return ~c;
}
static inline uint32_t live_config_u32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline void live_config_put(uint8_t *p,uint32_t v) {
    for(uint32_t i=0;i<4u;i++)p[i]=(uint8_t)(v>>(i*8u));
}
static inline void live_config_make(uint8_t out[64],uint64_t generation,uint32_t requested) {
    for(uint32_t i=0;i<64u;i++)out[i]=0;
    live_config_put(out,0x31504c46u);live_config_put(out+4,1u);
    live_config_put(out+8,0x00080311u);live_config_put(out+12,64u);
    live_config_put(out+16,(uint32_t)generation);live_config_put(out+20,(uint32_t)(generation>>32));
    live_config_put(out+24,requested);live_config_put(out+60,live_config_crc(out,60));
}
static inline int live_config_valid(const uint8_t *p,uint32_t bytes,uint64_t *generation,uint32_t *requested) {
    if(bytes!=64u || live_config_u32(p)!=0x31504c46u || live_config_u32(p+4)!=1u ||
       live_config_u32(p+8)!=0x00080311u || live_config_u32(p+12)!=64u ||
       live_config_u32(p+24)>1u || live_config_u32(p+60)!=live_config_crc(p,60))return 0;
    *generation=(uint64_t)live_config_u32(p+16)|((uint64_t)live_config_u32(p+20)<<32);
    *requested=live_config_u32(p+24);return 1;
}
#endif
