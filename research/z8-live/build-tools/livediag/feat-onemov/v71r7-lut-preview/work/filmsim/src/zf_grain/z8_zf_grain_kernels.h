/* SPDX-License-Identifier: MIT
 * Nikon Zf C3.01 Film Grain — Internal Scalar C11 & __aarch64__ NEON row kernels.
 * Every NEON kernel executes 100% bit-identical integer arithmetic to its Scalar
 * counterpart across all pixel values, edge cases, and arbitrary odd widths.
 */
#ifndef Z8_ZF_GRAIN_KERNELS_H
#define Z8_ZF_GRAIN_KERNELS_H

#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(ZF_GRAIN_SCALAR)
#define ZF_GRAIN_NEON 1
#else
#define ZF_GRAIN_NEON 0
#endif

/* Fast NEON-accelerated byte copy (100% SCTLR_EL1.A==1 safe for any alignment). */
void zf_grain_copy_u8_fast(uint8_t *dst, const uint8_t *src, uint32_t n);

/* Q4 map scaling (mapB == 0): out[i] = (int16_t)((blend_w * map_a[i]) >> 4) (exact ASR). */
void zf_grain_scale_map_s16(const int16_t *map_a, uint32_t count,
                            uint32_t blend_w, int16_t *out);

/* EXACT mode ApplyMaptoImageLine:
 *   Reads src_y[0..width-1], applies grain_map_row[x_idx[i]] with per-32px xorshift32
 *   polarity flip, multiplies by tone_lut[Y] >> 6 (floor ASR), clamps Y + delta to [0, 255].
 *   prng_state is the state immediately BEFORE the 32-pixel block containing global x0.
 *   bit_offset = global_x0 & 31U (0..31).
 *   std_knees != 0 indicates standard Zf knees (knee_lo==32 && knee_hi==223).
 */
uint32_t zf_grain_apply_line_exact(const uint8_t *src_y, uint8_t *dst_noisy,
                                   const int16_t *gm_row, const uint8_t *x_idx,
                                   const uint8_t *tone_lut, uint32_t width,
                                   uint32_t prng_state, uint32_t bit_offset,
                                   int std_knees);

/* SHARP mode ApplyMaptoImageLine:
 *   Computes signed noise delta[i] = round_sym((signed_grain * tone_lut[Y]), 6)
 *   into dst_delta[0..width-1] (int16_t) without modifying src_y.
 *   Returns updated PRNG state after processing all touched 32-pixel blocks.
 */
uint32_t zf_grain_apply_line_sharp(const uint8_t *src_y, int16_t *dst_delta,
                                   const int16_t *gm_row, const uint8_t *x_idx,
                                   const uint8_t *tone_lut, uint32_t width,
                                   uint32_t prng_state, uint32_t bit_offset,
                                   int std_knees);

/* SHARP mode ApplyMaptoImageLine with overflow-guarded int16x8 multiply fast path
 * (s16_mul_safe != 0 when max |gm_row| <= 511 -> |sg * tone| <= 32704 <= 32767).
 */
uint32_t zf_grain_apply_line_sharp_fast(const uint8_t *src_y, int16_t *dst_delta,
                                        const int16_t *gm_row, const uint8_t *x_idx,
                                        const uint8_t *tone_lut, uint32_t width,
                                        uint32_t prng_state, uint32_t bit_offset,
                                        int std_knees, int s16_mul_safe);

/* EXACT mode 3x3 binomial blur: [1,2,1; 2,4,2; 1,2,1] >> 4 (floor truncation, NO rounding).
 * r0, r1, r2 point to pixel 0 of each padded row (valid reads at [-1 .. width]).
 */
void zf_grain_blur3_u8_row(const uint8_t *r0, const uint8_t *r1, const uint8_t *r2,
                           uint8_t *out, uint32_t width);

/* EXACT mode 5x5 octagonal blur: {40,26,15,7,3,0} >> 8 (floor truncation, NO rounding).
 * r0..r4 point to pixel 0 of each padded row (valid reads at [-2 .. width + 1]).
 */
void zf_grain_blur5_u8_row(const uint8_t *r0, const uint8_t *r1, const uint8_t *r2,
                           const uint8_t *r3, const uint8_t *r4,
                           uint8_t *out, uint32_t width);

/* SHARP mode 3x3 signed int16 delta blur: [1,2,1; 2,4,2; 1,2,1] / 16 (symmetric unbiased round).
 * r0, r1, r2 point to pixel 0 of each padded row (valid reads at [-1 .. width]).
 */
void zf_grain_blur3_s16_row(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                            int16_t *out, uint32_t width);

/* SHARP mode 3x3 signed int16 delta blur fast int16x8 path (exact when max |r| <= 2047). */
void zf_grain_blur3_s16_row_fast(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                                 int16_t *out, uint32_t width);

/* SHARP mode 5x5 signed int16 delta blur: {40,26,15,7,3,0} / 256 (symmetric unbiased round).
 * r0..r4 point to pixel 0 of each padded row (valid reads at [-2 .. width + 1]).
 */
void zf_grain_blur5_s16_row(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                            const int16_t *r3, const int16_t *r4,
                            int16_t *out, uint32_t width);

/* SHARP mode 5x5 signed int16 delta blur fast int16x8->s32 path (exact when max |r| <= 4095). */
void zf_grain_blur5_s16_row_fast(const int16_t *r0, const int16_t *r1, const int16_t *r2,
                                 const int16_t *r3, const int16_t *r4,
                                 int16_t *out, uint32_t width);

/* SHARP mode final add-back: dst[i] = clamp(src_y[i] + delta[i], 0, 255). */
void zf_grain_add_delta_u8_row(const uint8_t *src_y, const int16_t *delta,
                               uint8_t *dst_y, uint32_t width);

#endif /* Z8_ZF_GRAIN_KERNELS_H */
