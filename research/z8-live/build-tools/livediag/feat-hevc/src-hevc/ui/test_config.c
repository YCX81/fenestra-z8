#include "live_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t a[65],b[65];uint64_t g=0u;uint32_t r=0u;
    live_config_make(a,7u,1u);
    assert(live_config_valid(a,64u,&g,&r)&&g==7u&&r==1u);
    for(uint32_t n=0;n<64u;n++)assert(!live_config_valid(a,n,&g,&r));
    a[64]=0u;assert(!live_config_valid(a,65u,&g,&r));
    for(uint32_t i=0;i<64u;i++)for(uint32_t bit=0;bit<8u;bit++){
        memcpy(b,a,64u);b[i]^=(uint8_t)(1u<<bit);assert(!live_config_valid(b,64u,&g,&r));
    }
    live_config_make(b,8u,0u);assert(live_config_valid(b,64u,&g,&r)&&g==8u&&r==0u);
    /* Every torn prefix of a newer write fails; previous slot remains valid. */
    uint8_t torn[64];for(uint32_t n=1u;n<64u;n++){
        memset(torn,0xff,64u);memcpy(torn,b,n);assert(!live_config_valid(torn,64u,&g,&r));
        assert(live_config_valid(a,64u,&g,&r)&&g==7u&&r==1u);
    }
    live_config_make(b,9u,2u);assert(!live_config_valid(b,64u,&g,&r));
    live_config_make(b,UINT64_MAX,1u);assert(live_config_valid(b,64u,&g,&r)&&g==UINT64_MAX);
    puts("PASS: roundtrip, 64 truncated sizes, trailing data, 512 bit flips, 63 torn writes, invalid requested, generation limit");
}
