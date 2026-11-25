#ifndef XM6_CYCLES_PORT_H
#define XM6_CYCLES_PORT_H

#include <stdint.h>
#include <ceva-time.h>

static inline void xm6_cycles_init(void)
{
    reset_clock();
    start_clock();
}

static inline uint64_t xm6_read_cycles_inline(void)
{
    uint32_t hi1 = (uint32_t)__builtin_xm6_vecc_in_cpm_sw_iCDP((const int *)0x314);
    uint32_t lo  = (uint32_t)__builtin_xm6_vecc_in_cpm_sw_iCDP((const int *)0x310);
    uint32_t hi2 = (uint32_t)__builtin_xm6_vecc_in_cpm_sw_iCDP((const int *)0x314);
    if (hi1 != hi2) {
        lo = (uint32_t)__builtin_xm6_vecc_in_cpm_sw_iCDP((const int *)0x310);
    }
    return ((uint64_t)hi2 << 32) | (uint64_t)lo;
}

#endif /* XM6_CYCLES_PORT_H */
