#ifndef LIVE_H264_IDR_H
#define LIVE_H264_IDR_H
#include <stddef.h>
#include <stdint.h>
/* Wave5 (C3.11 stock microcode) never emits IDR slices: a forced I picture is a
 * non-IDR (type 1) slice and frame_num/POC keep counting. This rewrites slice
 * headers so every I picture becomes an IDR and the following P pictures are
 * renumbered from it, giving closed GOPs that MOV sync samples can start at.
 * Slice data is CABAC (byte aligned after the header) and is copied unchanged. */
typedef struct {
    unsigned log2_max_frame_num, poc_type, log2_max_poc_lsb, frame_mbs_only;
    unsigned cabac, bottom_field_poc, redundant_pic_cnt, weighted_pred, weighted_bipred;
    unsigned deblocking_control, num_ref_idx_l0, num_ref_idx_l1, ready;
    unsigned base_frame_num, base_poc_lsb, have_base;
    uint8_t sps[256], pps[256];
    unsigned sps_n, pps_n;
} live_h264_idr;
/* sps/pps: NAL payloads starting at the NAL header byte (no start code). */
int live_h264_idr_init(live_h264_idr *, const uint8_t *sps, size_t sps_n, const uint8_t *pps, size_t pps_n);
/* One Annex-B access unit in, one out (out capacity >= in + 64). AUD/SEI NALs are
 * dropped; *idr is set when the AU now starts a closed GOP. Returns 0 on success. */
int live_h264_idr_au(live_h264_idr *, const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                     size_t *out_n, unsigned *idr);
#endif
