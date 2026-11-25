/* Strict CEVA-XM6 NikonDMA hardware-behavior mock for dcli.exe verification.
 * Intercepts the EXACT stock Z8 C3.11 NikonDMA C++ symbols called by
 * zf_grain_xm6_opt.cpp and ricoh_hdf_xm6_opt.cpp compiled with
 * -DXM6_TARGET_Z8_ELF0 -DXM6_USE_NIKON_DMA, asserting:
 *   1. Descriptor 32B zero-initialized before CreateDescriptor1D/2D
 *   2. Channel/direction match (Ch0=E2I, Ch1=I2E) and mode==0
 *   3. Lengths/strides in [1..1,048,575] and 4B-aligned (& 3 == 0)
 *   4. All src & dst addresses 4B-aligned (& 3 == 0)
 *   5. Strict IDM & EXT memory region bounds
 *   6. Valid monotonic sync tokens and zero unwaited writes at kernel exit
 */
#include "nikondma_mock.h"
#include <string.h>

typedef struct {
    uint32_t magic;      /* w[0] */
    uint32_t ch;         /* w[1] */
    uint32_t dir;        /* w[2]: 0=E2I, 1=I2E */
    uint32_t is_2d;      /* w[3] */
    uint32_t width_len;  /* w[4] */
    uint32_t height;     /* w[5] */
    uint32_t src_stride; /* w[6] */
    uint32_t dst_stride; /* w[7] */
} MockDesc;

#define MOCK_DESC_MAGIC 0x4E444D41U /* 'NDMA' */

NikonDmaAudit g_ndma_audit;

void nikondma_mock_reset(uintptr_t idm_lo, uintptr_t idm_hi)
{
    memset(&g_ndma_audit, 0, sizeof(g_ndma_audit));
    g_ndma_audit.idm_reg.lo = idm_lo;
    g_ndma_audit.idm_reg.hi = idm_hi;
}

void nikondma_mock_add_ext(uintptr_t ext_lo, uintptr_t ext_hi)
{
    if (g_ndma_audit.num_ext_regs < 8U) {
        g_ndma_audit.ext_regs[g_ndma_audit.num_ext_regs].lo = ext_lo;
        g_ndma_audit.ext_regs[g_ndma_audit.num_ext_regs].hi = ext_hi;
        g_ndma_audit.num_ext_regs++;
    }
}

uint32_t nikondma_mock_total_errors(void)
{
    return g_ndma_audit.err_desc_not_zero +
           g_ndma_audit.err_bad_ch_dir +
           g_ndma_audit.err_bad_size +
           g_ndma_audit.err_align_mod4 +
           g_ndma_audit.err_len_mod4 +
           g_ndma_audit.err_idm_oob +
           g_ndma_audit.err_ext_oob +
           g_ndma_audit.err_bad_token;
}

uint32_t nikondma_mock_unwaited_writes(void)
{
    return (g_ndma_audit.issued_tok[1] > g_ndma_audit.waited_tok[1])
               ? (g_ndma_audit.issued_tok[1] - g_ndma_audit.waited_tok[1])
               : 0U;
}

static int check_zero(const MockDesc *d)
{
    const uint32_t *w = (const uint32_t *)d;
    for (int i = 0; i < 8; ++i) {
        if (w[i] != 0U) return 0;
    }
    return 1;
}

static int in_idm(uintptr_t a, uint32_t bytes)
{
    return (a >= g_ndma_audit.idm_reg.lo && (a + bytes) <= g_ndma_audit.idm_reg.hi);
}

static int in_ext(uintptr_t a, uint32_t bytes)
{
    for (uint32_t i = 0U; i < g_ndma_audit.num_ext_regs; ++i) {
        if (a >= g_ndma_audit.ext_regs[i].lo && (a + bytes) <= g_ndma_audit.ext_regs[i].hi) {
            return 1;
        }
    }
    return 0;
}

void _ZN8NikonDMA18CreateDescriptor1DEPNS_10DescriptorEjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE(
    void *desc_p, uint32_t ch, uint32_t length, uint32_t dir, uint32_t mode)
{
    MockDesc *d = (MockDesc *)desc_p;
    if (!check_zero(d)) g_ndma_audit.err_desc_not_zero++;
    if (ch > 1U || dir > 1U || ch != dir || mode != 0U) g_ndma_audit.err_bad_ch_dir++;
    if (length == 0U || length > 1048575U) g_ndma_audit.err_bad_size++;
    if (length & 3U) g_ndma_audit.err_len_mod4++;
    if (length & 15U) g_ndma_audit.non16_count++;

    d->magic      = MOCK_DESC_MAGIC;
    d->ch         = ch;
    d->dir        = dir;
    d->is_2d      = 0U;
    d->width_len  = length;
    d->height     = 1U;
    d->src_stride = length;
    d->dst_stride = length;
}

void _ZN8NikonDMA18CreateDescriptor2DEPNS_10DescriptorEjjjjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE(
    void *desc_p, uint32_t ch, uint32_t width_bytes, uint32_t height,
    uint32_t src_stride, uint32_t dst_stride, uint32_t dir, uint32_t mode)
{
    MockDesc *d = (MockDesc *)desc_p;
    if (!check_zero(d)) g_ndma_audit.err_desc_not_zero++;
    if (ch > 1U || dir > 1U || ch != dir || mode != 0U) g_ndma_audit.err_bad_ch_dir++;
    if (width_bytes == 0U || width_bytes > 1048575U || height == 0U || height > 1048575U)
        g_ndma_audit.err_bad_size++;
    if ((width_bytes & 3U) || (src_stride & 3U) || (dst_stride & 3U))
        g_ndma_audit.err_len_mod4++;

    d->magic      = MOCK_DESC_MAGIC;
    d->ch         = ch;
    d->dir        = dir;
    d->is_2d      = 1U;
    d->width_len  = width_bytes;
    d->height     = height;
    d->src_stride = src_stride;
    d->dst_stride = dst_stride;
}

static void exec_xfer(void *dst, const void *src, const MockDesc *d)
{
    if (d->magic != MOCK_DESC_MAGIC) {
        g_ndma_audit.err_desc_not_zero++;
        return;
    }
    if (d->is_2d) {
        if (d->dir == 0U) g_ndma_audit.calls_2d_e2i++; else g_ndma_audit.calls_2d_i2e++;
    } else {
        if (d->dir == 0U) g_ndma_audit.calls_1d_e2i++; else g_ndma_audit.calls_1d_i2e++;
    }
    for (uint32_t r = 0U; r < d->height; ++r) {
        uintptr_t dp = (uintptr_t)dst + (uintptr_t)r * d->dst_stride;
        uintptr_t sp = (uintptr_t)src + (uintptr_t)r * d->src_stride;
        if ((dp & 3U) || (sp & 3U)) g_ndma_audit.err_align_mod4++;
        if ((dp & 15U) || (sp & 15U)) g_ndma_audit.non16_count++;
        if (d->dir == 0U) { /* E2I: src=EXT, dst=IDM */
            if (!in_idm(dp, d->width_len)) g_ndma_audit.err_idm_oob++;
            if (!in_ext(sp, d->width_len)) g_ndma_audit.err_ext_oob++;
        } else { /* I2E: src=IDM, dst=EXT */
            if (!in_idm(sp, d->width_len)) g_ndma_audit.err_idm_oob++;
            if (!in_ext(dp, d->width_len)) g_ndma_audit.err_ext_oob++;
        }
        memcpy((void *)dp, (const void *)sp, d->width_len);
    }
}

void _ZN8NikonDMA21DispatchRequestNoSyncEPvPKvPKNS_10DescriptorE(
    void *dst, const void *src, const void *desc_p)
{
    exec_xfer(dst, src, (const MockDesc *)desc_p);
}

uint32_t _ZN8NikonDMA15DispatchRequestEPvPKvPKNS_10DescriptorE(
    void *dst, const void *src, const void *desc_p)
{
    const MockDesc *d = (const MockDesc *)desc_p;
    exec_xfer(dst, src, d);
    uint32_t ch = (d->ch <= 1U) ? d->ch : 0U;
    g_ndma_audit.issued_tok[ch]++;
    return g_ndma_audit.issued_tok[ch];
}

void _ZN8NikonDMA11WaitRequestEjj(uint32_t ch, uint32_t sync_token)
{
    g_ndma_audit.calls_wait++;
    if (ch > 1U || sync_token > g_ndma_audit.issued_tok[ch]) {
        g_ndma_audit.err_bad_token++;
        return;
    }
    if (sync_token > g_ndma_audit.waited_tok[ch]) {
        g_ndma_audit.waited_tok[ch] = sync_token;
    }
}
