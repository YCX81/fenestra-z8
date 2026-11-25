/* SPDX-License-Identifier: MIT
 * Pixel & multi-scale optical scatter kernels for z8_hdf.
 * Scalar C11 and AArch64 NEON produce 100% bit-for-bit identical results.
 */
#ifndef Z8_HDF_KERNELS_H
#define Z8_HDF_KERNELS_H

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(HDF_SCALAR)
#define HDF_NEON 1
#else
#define HDF_NEON 0
#endif

/* Automatic symbol-namespace isolation when dual-linking NEON + Scalar objects in a
 * single test binary that defines -DHDF_SCALAR -Dhdf_ycc_to_rgb_row=... (zero effect
 * on firmware builds where hdf_ycc_to_rgb_row is not macro-renamed). */
#if defined(HDF_SCALAR) && defined(hdf_ycc_to_rgb_row)
#ifndef hdf_s1_subsample_ycc_to_rgb_row
#define hdf_s1_subsample_ycc_to_rgb_row scalar_hdf_s1_subsample_ycc_to_rgb_row
#endif
#ifndef hdf_accum_blocks_row_sub
#define hdf_accum_blocks_row_sub        scalar_hdf_accum_blocks_row_sub
#endif
#ifndef hdf_blend_3scales_k_row_u16
#define hdf_blend_3scales_k_row_u16     scalar_hdf_blend_3scales_k_row_u16
#endif
#ifndef hdf_still_composite_row_fast
#define hdf_still_composite_row_fast    scalar_hdf_still_composite_row_fast
#endif
#ifndef hdf_live_s3_comp_y_rowpair_w
#define hdf_live_s3_comp_y_rowpair_w    scalar_hdf_live_s3_comp_y_rowpair_w
#endif
#ifndef hdf_live_evf_s1_rowgroup_256
#define hdf_live_evf_s1_rowgroup_256    scalar_hdf_live_evf_s1_rowgroup_256
#endif
#ifndef hdf_live_evf_hlerp_256_to_640
#define hdf_live_evf_hlerp_256_to_640   scalar_hdf_live_evf_hlerp_256_to_640
#endif
#ifndef hdf_live_evf_s3_apply_chroma_256_to_640
#define hdf_live_evf_s3_apply_chroma_256_to_640 scalar_hdf_live_evf_s3_apply_chroma_256_to_640
#endif
#ifndef hdf_live_evf_s3_chroma_exact_masked
#define hdf_live_evf_s3_chroma_exact_masked scalar_hdf_live_evf_s3_chroma_exact_masked
#endif
#endif

/* BT.601 full-range integer coefficients (Q8) */
#define HDF_RGB_R_CR 359
#define HDF_RGB_G_CB  88
#define HDF_RGB_G_CR 183
#define HDF_RGB_B_CB 454

#define HDF_YCC_Y_R   77
#define HDF_YCC_Y_G  150
#define HDF_YCC_Y_B   29
#define HDF_YCC_CB_R  43
#define HDF_YCC_CB_G  85
#define HDF_YCC_CB_B 128
#define HDF_YCC_CR_R 128
#define HDF_YCC_CR_G 107
#define HDF_YCC_CR_B  21

/* Highlight extraction & shoulder constants */
#define HDF_CORE_KNEE_Q16    53248U
#define HDF_SHOULDER_K1_Q16  51200U
#define HDF_SHOULDER_K2_Q16  71680U
#define HDF_SHOULDER_TOP_Q16 104448U

/* Chroma write mode for hdf_composite_row */
enum hdf_chroma_mode {
    HDF_CHROMA_422_DIRECT = 0, /* NV16 4:2:2 row, or lone final odd-height NV12 row */
    HDF_CHROMA_420_EVEN   = 1, /* NV12 even row: stash dcb/dcr, leave c_row intact */
    HDF_CHROMA_420_ODD    = 2  /* NV12 odd row: average with stashed even dcb/dcr & commit */
};

typedef struct {
    uint32_t knee_q16;
    uint32_t boost_quad_q14;
    uint32_t boost_core_q14;
    uint32_t ev_gain_q8;
    uint32_t neutrality_q8; /* 0..256 */
} hdf_extract_params;

extern const uint16_t hdf_srgb_to_lin_u16[256];

uint8_t hdf_lin_to_srgb_scalar(uint16_t lin_q16);

/* 1. Full-range BT.601 YCbCr (interleaved Cb,Cr pairs) -> planar 8-bit sRGB */
void hdf_ycc_to_rgb_row(const uint8_t *y_row, const uint8_t *c_row,
                        uint8_t *r_out, uint8_t *g_out, uint8_t *b_out,
                        uint32_t width, uint32_t monochrome);

/* 1b. V71.5 Strided Stage 1 YCbCr -> planar 8-bit sRGB (step=2 or step=4, outputs width/step px) */
void hdf_s1_subsample_ycc_to_rgb_row(const uint8_t *y_row, const uint8_t *c_row,
                                     uint8_t *r_out, uint8_t *g_out, uint8_t *b_out,
                                     uint32_t width, uint32_t step, uint32_t monochrome);

/* 2. Planar 8-bit sRGB -> 16-bit Q16 linear light via 256-entry .rodata LUT */
void hdf_srgb_to_lin_row(const uint8_t *srgb, uint16_t *lin_out, uint32_t count);

/* 3. Linear-light RGB -> Q14 highlight-boosted optical scatter radiance E */
void hdf_extract_energy_row(const uint16_t *lr, const uint16_t *lg, const uint16_t *lb,
                            uint16_t *er, uint16_t *eg, uint16_t *eb,
                            uint32_t count, const hdf_extract_params *params);

/* 4. Full-coverage horizontal BxB block accumulation (fixes stride-skipping phase bug) */
void hdf_accum_blocks_row(const uint16_t *er, const uint16_t *eg, const uint16_t *eb,
                          uint32_t *acc_r, uint32_t *acc_g, uint32_t *acc_b,
                          uint32_t width, uint32_t block_size);

/* 4b. V71.5 Strided subsampled block accumulation (step=2 or step=4) */
void hdf_accum_blocks_row_sub(const uint16_t *er, const uint16_t *eg, const uint16_t *eb,
                              uint32_t *acc_r, uint32_t *acc_g, uint32_t *acc_b,
                              uint32_t width, uint32_t block_size, uint32_t step);

/* 5. Exact 2x2 area-average pyramid downsample on u16 plane */
void hdf_downsample_2x2_u16(const uint16_t *src, uint32_t src_w, uint32_t src_h, uint32_t src_stride,
                            uint16_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride);

/* 6. Separable symmetric (2*r + 1)-tap box blur passes (exact integer rounding) */
void hdf_blur_h_row_u16(const uint16_t *src_row, uint16_t *dst_row, uint32_t width, uint32_t radius);
void hdf_blur_h_grid_u16(uint16_t *grid, uint16_t *h_tmp,
                         uint32_t width, uint32_t height, uint32_t stride, uint32_t radius);
void hdf_blur_v_grid_u16(uint16_t *grid, uint16_t *col_scratch,
                         uint32_t width, uint32_t height, uint32_t stride, uint32_t radius);

/* 7. Separable bilinear upsampling & multi-scale weighted blend */
void hdf_vlerp_row_u16(const uint16_t *row0, const uint16_t *row1,
                       uint16_t *vrow_out, uint32_t gw, uint32_t fy_q8, uint32_t pad_tail);

void hdf_hlerp_row_u16(const uint16_t *vrow, const uint16_t *gx_arr, const uint8_t *fx_arr,
                       uint16_t *out, uint32_t width);

void hdf_hlerp_rgb_row_u16(const uint16_t *vr, const uint16_t *vg, const uint16_t *vb,
                           const uint16_t *gx_arr, const uint8_t *fx_arr,
                           uint16_t *sr, uint16_t *sg, uint16_t *sb, uint32_t width);

void hdf_blend_3scales_row_u16(uint16_t *s1_inout, const uint16_t *s2_row, const uint16_t *s3_row,
                               uint32_t gw, uint32_t w1, uint32_t w2, uint32_t w3,
                               uint32_t white_scale_q8);

/* 7b. V71.5 3-scale blend + white_scale + k_q12 fold into Q15 scatter sk = (mix*k + 1024) >> 11 */
void hdf_blend_3scales_k_row_u16(uint16_t *s1_inout, const uint16_t *s2_row, const uint16_t *s3_row,
                                 uint32_t gw, uint32_t w1, uint32_t w2, uint32_t w3,
                                 uint32_t white_scale_q8, uint32_t k_q12);

/* 8. Linear-light optical scatter + soft highlight shoulder composite onto YCbCr row */
void hdf_composite_row(uint8_t *y_row, uint8_t *c_row,
                       const uint16_t *sr_q14, const uint16_t *sg_q14, const uint16_t *sb_q14,
                       int16_t *dcb_stash, int16_t *dcr_stash,
                       uint32_t width, uint32_t k_q12,
                       uint32_t monochrome, uint32_t chroma_mode);

/* 8b. V71.5 Fused Still Stage 3 row kernel: L1-tiled Q15 hlerp + zero-halo/dark fast-path +
 * live_knot16 + cold shoulder + live_clz_umull64 + sqrdmulh YCC delta (0 Q-spills, 0-diff NEON==Scalar).
 * tile_scratch must be 16-byte aligned and >= HDF_STILL_TILE_SCRATCH_BYTES (6144 B).
 */
#define HDF_STILL_TILE_PX            512U
#define HDF_STILL_TILE_SCRATCH_BYTES (HDF_STILL_TILE_PX * 12U) /* 3*u16 + 3*u8 + 3*u8 = 6144 B */

void hdf_still_composite_row_fast(uint8_t *y_row, uint8_t *c_row,
                                  const uint16_t *vr_q15, const uint16_t *vg_q15, const uint16_t *vb_q15,
                                  const uint16_t *gx_arr, const uint8_t *fx_arr,
                                  int16_t *dcb_stash, int16_t *dcr_stash,
                                  uint8_t *tile_scratch, uint32_t width,
                                  uint32_t level, uint32_t monochrome, uint32_t chroma_mode);

/* 9. Live-specialized 1024x{680,682} NV12 (LIVE_256_YC) Q15/Q13 row & grid kernels.
 * Scalar C11 (-DHDF_SCALAR) and AArch64 NEON produce bit-identical results.
 */
void hdf_live_s1_rowpair_256(const uint8_t *y0, const uint8_t *y1, const uint8_t *c0,
                             const uint8_t *y2, const uint8_t *y3, const uint8_t *c1,
                             const uint8_t *boost_lo, const uint8_t *boost_hi,
                             uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out,
                             uint8_t *r256_out, uint8_t *g256_out, uint8_t *b256_out,
                             uint32_t *c_edge_mask_out, uint32_t *y_edge_mask_out,
                             uint8_t *boost_scratch, uint32_t monochrome);

void hdf_live_ds2x2_u16(const uint16_t *src, uint32_t sw, uint32_t sh,
                        uint16_t *dst, uint32_t dw, uint32_t dh);

void hdf_live_blur_sym53_2d(uint16_t *grid, uint16_t *tmp, uint32_t w, uint32_t h);

void hdf_live_blur_box16_1pass(uint16_t *grid, uint16_t *tmp_grid, int16_t *htmp,
                               uint32_t w, uint32_t h, uint32_t r);

void hdf_live_vlerp_q13_padded(const uint16_t *r0, const uint16_t *r1,
                               int16_t *buf_with_pad, uint32_t sw, uint32_t fy_q8);

void hdf_live_vlerp_hlerp_y_512(const uint16_t *r0, const uint16_t *r1,
                                uint16_t *out_512, uint32_t fy_q8);

void hdf_live_s2_blend_k_row_256(uint16_t *row0,
                                 const int16_t *vrow1_padded, const int16_t *vrow2_padded,
                                 int16_t w1k, int16_t w2k, int16_t w3k);

void hdf_live_build_grid_y(const uint16_t *grid_r, const uint16_t *grid_g,
                           const uint16_t *grid_b, uint16_t *grid_y,
                           uint32_t total_cells);

uint32_t hdf_live_s3_check_schroma_mask(const uint16_t *gr, const uint16_t *gg,
                                        const uint16_t *gb, uint32_t y_check_mask);

void hdf_live_s3_comp_chroma_256(const uint8_t *rin, const uint8_t *gin, const uint8_t *bin,
                                 uint8_t *rout, uint8_t *gout, uint8_t *bout,
                                 const uint16_t *gr, const uint16_t *gg, const uint16_t *gb,
                                 uint32_t level);

void hdf_live_s3_apply_chroma_256_2x2(uint8_t *c0, uint8_t *c1,
                                      const uint8_t *rin, const uint8_t *gin, const uint8_t *bin,
                                      const uint8_t *rout, const uint8_t *gout, const uint8_t *bout);

void hdf_live_s3_chroma_exact_masked(const uint8_t *y0, const uint8_t *y1, uint8_t *c0,
                                     const uint8_t *y2, const uint8_t *y3, uint8_t *c1,
                                     const uint16_t *gr, const uint16_t *gg, const uint16_t *gb,
                                     uint32_t mask, uint32_t level,
                                     uint8_t *fb_buf, uint8_t *exact_uv_out);

void hdf_live_s3_patch_exact_uv(uint8_t *c0, uint8_t *c1,
                                const uint8_t *exact_uv, uint32_t mask);

void hdf_live_s3_comp_y_rowpair(uint8_t *yr0, uint8_t *yr1,
                                const uint16_t *s_y_512, uint32_t level);

void hdf_live_s3_comp_y_rowpair_w(uint8_t *yr0, uint8_t *yr1,
                                  const uint16_t *s_y_half, uint32_t width, uint32_t level);

/* 10. EVF-specialized 1280x{850..856} NV12 fast HDF kernels (256x171 Q13 pyramid
 *     + crisp native 1280x852 composite; bit-exact NEON == Scalar, 0 Q-spills). */
void hdf_live_evf_s1_rowgroup_256(const uint8_t *const *y_rows,
                                  const uint8_t *const *c_rows,
                                  uint32_t n_crows,
                                  const uint8_t *boost_lo, const uint8_t *boost_hi,
                                  uint16_t *gr_out, uint16_t *gg_out, uint16_t *gb_out,
                                  uint8_t *r256_out, uint8_t *g256_out, uint8_t *b256_out,
                                  uint32_t *c_edge_mask_out, uint32_t *y_edge_mask_out,
                                  uint8_t *boost_scratch,
                                  uint8_t *evf_y640, uint8_t *evf_cb640, uint8_t *evf_cr640,
                                  uint32_t monochrome);

void hdf_live_evf_hlerp_256_to_640(const int16_t *vrow_padded, uint16_t *out_640);

void hdf_live_evf_s3_apply_chroma_256_to_640(uint8_t *const *c_rows, uint32_t n_crows,
                                             const uint8_t *rin, const uint8_t *gin, const uint8_t *bin,
                                             const uint8_t *rout, const uint8_t *gout, const uint8_t *bout,
                                             uint32_t skip_mask);

void hdf_live_evf_s3_chroma_exact_masked(const uint8_t *const *y_rows,
                                         uint8_t *const *c_rows,
                                         uint32_t n_crows,
                                         const uint16_t *gr, const uint16_t *gg, const uint16_t *gb,
                                         uint32_t mask, uint32_t level,
                                         uint8_t *fb_buf);

#endif /* Z8_HDF_KERNELS_H */
