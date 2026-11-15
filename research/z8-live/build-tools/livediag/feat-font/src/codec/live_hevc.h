#ifndef LIVE_HEVC_H
#define LIVE_HEVC_H
#include <stddef.h>
#include <stdint.h>
/* Photo-mode Live with the Wave5 HEVC path (codec word 1). The encoder runs low-delay
 * IP; random access comes from its own IRAP pictures (types 16..21). Parameter sets are
 * taken once from HEADERS; in-band copies are checked against them and dropped, and the
 * stored (display-cropped) VPS/SPS/PPS are put in front of every IRAP so any IRAP can
 * start a MOV. AUD/SEI are dropped. Counters are for USB readout. */
typedef struct {
    uint8_t vps[128], sps[256], pps[128], sps_raw[256];
    unsigned vps_n, sps_n, pps_n, sps_raw_n, ready, cropped, width, height;
    uint32_t n_au, n_irap, n_idr, n_cra, n_trail, n_ps_inband, n_ps_mismatch, n_dropped_nal, last_vcl;
} live_hevc;
/* One NAL (2-byte header, no start code) from HEADERS. Unknown types are ignored. */
int live_hevc_param(live_hevc *, const uint8_t *nal, size_t n, unsigned width, unsigned height);
/* One Annex-B AU in, one out (cap >= n + 512). *irap set when the AU can start a clip. */
int live_hevc_au(live_hevc *, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_n, unsigned *irap);
/* SPS display crop: coded W x H (no window) -> conformance window to w x h (4:2:0,
 * single sub-layer, 8-bit). Returns 0 with *written, -1 leaves out untouched. */
int live_hevc_sps_crop(const uint8_t *nal, size_t n, unsigned w, unsigned h, uint8_t *out, size_t cap, size_t *written);
#endif
