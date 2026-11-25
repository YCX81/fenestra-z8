#include "ricoh_hdf_xm6_v2.h"
#include "xm6_cycles_port.h"
#include "nikondma_mock.h"
#include "hdf_matrix_cases.h"
#include "hdf_expected.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define EXT_TEST_SEC __attribute__((section(".ext_test_bufs"), aligned(64)))
#define MAX_PLANE_BYTES (1280U * 856U) /* 1,095,680 B (>= 8256*128 = 1,056,768 B) */

static uint8_t g_ext_ws[Z8_XM6_HDF_EXT_BYTES] EXT_TEST_SEC;
static uint8_t g_src_y[MAX_PLANE_BYTES]       EXT_TEST_SEC;
static uint8_t g_src_c[MAX_PLANE_BYTES]       EXT_TEST_SEC;
static uint8_t g_dst_y[MAX_PLANE_BYTES]       EXT_TEST_SEC;
static uint8_t g_dst_c[MAX_PLANE_BYTES]       EXT_TEST_SEC;

#ifndef CASE_START
#define CASE_START 0U
#endif
#ifndef CASE_END
#define CASE_END NUM_HDF_CASES
#endif

int main(void)
{
    xm6_cycles_init();
    uint32_t passed = 0U, failed = 0U;
    uint32_t c_end = CASE_END;
    if (c_end > NUM_HDF_CASES) c_end = NUM_HDF_CASES;

    /* Stamp canary bytes in stock FW_EXT_WORKBUF 0x41930000..0x419b0000 to PROVE
     * Z8_RicohHdf_Slot41 honors in->ext_work_addr != 0 and NEVER touches tsk0esc/tsk1esc! */
    volatile uint32_t *fw_ext_canary = (volatile uint32_t *)(uintptr_t)0x41930000U;
    fw_ext_canary[0]          = 0xDEADBEEFU; /* _ZL17m_XmDmaWriteQueue */
    fw_ext_canary[0x10]       = 0xCAFEBABEU; /* 0x41930040 */
    fw_ext_canary[0x10000/4]  = 0x11223344U; /* 0x41940000 tsk0esc */
    fw_ext_canary[0x40000/4]  = 0x55667788U; /* 0x41970000 tsk1esc */

    for (uint32_t i = CASE_START; i < c_end; ++i) {
        const HdfMatrixCase *tc = &k_hdf_cases[i];
        uint32_t ch = tc->is_nv12 ? ((tc->h + 1U) >> 1) : tc->h;
        size_t yb = (size_t)tc->y_stride * tc->h;
        size_t cb = (size_t)tc->c_stride * ch;

        hdf_fill_pattern(g_src_y, g_src_c, tc->w, tc->h, tc->y_stride, tc->c_stride, tc->is_nv12, tc->pattern);
        if (!tc->in_place) {
            memset(g_dst_y, 0x5A, yb);
            memset(g_dst_c, 0x5A, cb);
        }
        memset((void *)(uintptr_t)Z8_XM6_HDF_IDM_ADDR, 0xCC, Z8_XM6_HDF_IDM_BYTES);
        memset(g_ext_ws, 0xDD, sizeof(g_ext_ws));

        Z8Xm6HdfParams in;
        uint8_t mode = (tc->is_live == 2U) ? Z8_XM6_HDF_MODE_LIVE_EVF_256_YC
                     : ((tc->is_live == 1U) ? Z8_XM6_HDF_MODE_LIVE_256_YC
                                            : Z8_XM6_HDF_MODE_STILL_GENERAL);
        z8_xm6_hdf_init_params(&in, tc->level, tc->mono, mode);
        in.src.plane_addr[0] = (uint64_t)(uintptr_t)g_src_y;
        in.src.plane_addr[1] = (uint64_t)(uintptr_t)g_src_c;
        in.src.width         = (int32_t)tc->w;
        in.src.height        = (int32_t)tc->h;
        in.src.stride[0]     = (int32_t)tc->y_stride;
        in.src.stride[1]     = (int32_t)tc->c_stride;
        in.src.format        = tc->is_nv12 ? Z8_XM6_FMT_NV12 : Z8_XM6_FMT_NV16;
        in.white[0]          = tc->white[0];
        in.white[1]          = tc->white[1];
        in.white[2]          = tc->white[2];
        in.knee_q16          = tc->knee_q16;
        in.neutrality_q8     = tc->neutrality_q8;
        if (tc->exposure_known) {
            in.ev_gain_q8 = z8_xm6_hdf_ev_gain_from_ev256(tc->scene_ev256);
        }
        /* Always pass dedicated 512 KiB external workspace (matches EL1 P0 fix!) */
        in.ext_work_addr  = (uint64_t)(uintptr_t)g_ext_ws;
        in.ext_work_bytes = sizeof(g_ext_ws);

        Z8Xm6HdfOutParam out = in.src;
        uint8_t *oy = tc->in_place ? g_src_y : g_dst_y;
        uint8_t *oc = tc->in_place ? g_src_c : g_dst_c;
        out.plane_addr[0] = (uint64_t)(uintptr_t)oy;
        out.plane_addr[1] = (uint64_t)(uintptr_t)oc;

        nikondma_mock_reset(Z8_XM6_HDF_IDM_ADDR, Z8_XM6_HDF_IDM_ADDR + Z8_XM6_HDF_IDM_BYTES);
        nikondma_mock_add_ext((uintptr_t)g_ext_ws, (uintptr_t)g_ext_ws + sizeof(g_ext_ws));
        nikondma_mock_add_ext((uintptr_t)g_src_y,  (uintptr_t)g_src_y  + yb);
        nikondma_mock_add_ext((uintptr_t)g_src_c,  (uintptr_t)g_src_c  + cb);
        nikondma_mock_add_ext((uintptr_t)g_dst_y,  (uintptr_t)g_dst_y  + yb);
        nikondma_mock_add_ext((uintptr_t)g_dst_c,  (uintptr_t)g_dst_c  + cb);

        uint64_t c0 = xm6_read_cycles_inline();
        int32_t rc = Z8_RicohHdf_Slot41(&in, &out);
        uint64_t c1 = xm6_read_cycles_inline();

        uint64_t yh = hdf_fnv1a_rows(oy, tc->w, tc->h, tc->y_stride);
        uint64_t chash = hdf_fnv1a_rows(oc, tc->w, ch, tc->c_stride);

        uint32_t pad_err = 0U;
        for (uint32_t r = 0U; r < tc->h; ++r) {
            for (uint32_t x = tc->w; x < tc->y_stride; ++x) {
                if (oy[(size_t)r * tc->y_stride + x] != 0x5AU) ++pad_err;
            }
        }
        for (uint32_t r = 0U; r < ch; ++r) {
            for (uint32_t x = tc->w; x < tc->c_stride; ++x) {
                if (oc[(size_t)r * tc->c_stride + x] != 0x5AU) ++pad_err;
            }
        }

        uint32_t canary_err = (fw_ext_canary[0] != 0xDEADBEEFU) +
                              (fw_ext_canary[0x10] != 0xCAFEBABEU) +
                              (fw_ext_canary[0x10000/4] != 0x11223344U) +
                              (fw_ext_canary[0x40000/4] != 0x55667788U);
        uint32_t dma_errs = nikondma_mock_total_errors();
        uint32_t unwaited = nikondma_mock_unwaited_writes();
        uint32_t total_dma = g_ndma_audit.calls_1d_e2i + g_ndma_audit.calls_1d_i2e +
                             g_ndma_audit.calls_2d_e2i + g_ndma_audit.calls_2d_i2e;

        int ok = (rc == 0) &&
                 (yh == k_hdf_arm_hashes[i].y_hash) &&
                 (chash == k_hdf_arm_hashes[i].c_hash) &&
                 (pad_err == 0U) &&
                 (canary_err == 0U) &&
                 (dma_errs == 0U) &&
                 (unwaited == 0U) &&
                 (total_dma > 0U);
        if (ok) ++passed; else ++failed;

        printf("[HDF %2u] %-28s %s rc=%d cyc=%llu dma=%u(1d:%u/%u,2d:%u/%u) err=%u unw=%u can=%u pad=%u\n",
               i, tc->name, ok ? "PASS" : "FAIL", (int)rc,
               (unsigned long long)(c1 - c0), total_dma,
               g_ndma_audit.calls_1d_e2i, g_ndma_audit.calls_1d_i2e,
               g_ndma_audit.calls_2d_e2i, g_ndma_audit.calls_2d_i2e,
               dma_errs, unwaited, canary_err, pad_err);
        if (!ok) {
            printf("         EXP Y=%016llx C=%016llx GOT Y=%016llx C=%016llx\n",
                   (unsigned long long)k_hdf_arm_hashes[i].y_hash,
                   (unsigned long long)k_hdf_arm_hashes[i].c_hash,
                   (unsigned long long)yh, (unsigned long long)chash);
            printf("         DMA detail: desc=%u ch=%u sz=%u al4=%u len4=%u idm=%u ext=%u tok=%u\n",
                   g_ndma_audit.err_desc_not_zero, g_ndma_audit.err_bad_ch_dir,
                   g_ndma_audit.err_bad_size, g_ndma_audit.err_align_mod4,
                   g_ndma_audit.err_len_mod4, g_ndma_audit.err_idm_oob,
                   g_ndma_audit.err_ext_oob, g_ndma_audit.err_bad_token);
        }
        fflush(stdout);
    }
    printf("HDF_NIKONDMA_SUMMARY [%u..%u): PASS=%u FAIL=%u\n", CASE_START, c_end, passed, failed);
    fflush(stdout);
    return failed ? 1 : 0;
}
