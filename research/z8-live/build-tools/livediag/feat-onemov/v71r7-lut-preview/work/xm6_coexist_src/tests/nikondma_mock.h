#ifndef NIKONDMA_MOCK_H
#define NIKONDMA_MOCK_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uintptr_t lo;
    uintptr_t hi;
} DmaRegion;

typedef struct {
    DmaRegion idm_reg;
    DmaRegion ext_regs[8];
    uint32_t  num_ext_regs;

    uint32_t  calls_1d_e2i;
    uint32_t  calls_1d_i2e;
    uint32_t  calls_2d_e2i;
    uint32_t  calls_2d_i2e;
    uint32_t  calls_wait;

    uint32_t  err_desc_not_zero;
    uint32_t  err_bad_ch_dir;
    uint32_t  err_bad_size;
    uint32_t  err_align_mod4;
    uint32_t  err_len_mod4;
    uint32_t  err_idm_oob;
    uint32_t  err_ext_oob;
    uint32_t  err_bad_token;
    uint32_t  non16_count;

    uint32_t  issued_tok[2];
    uint32_t  waited_tok[2];
} NikonDmaAudit;

#ifdef __cplusplus
extern "C" {
#endif

extern NikonDmaAudit g_ndma_audit;

void nikondma_mock_reset(uintptr_t idm_lo, uintptr_t idm_hi);
void nikondma_mock_add_ext(uintptr_t ext_lo, uintptr_t ext_hi);
uint32_t nikondma_mock_total_errors(void);
uint32_t nikondma_mock_unwaited_writes(void);

#ifdef __cplusplus
}
#endif
#endif
