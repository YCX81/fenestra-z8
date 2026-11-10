/* Emulation-only preview kernel. No GUI, filesystem or native task ownership. */
#ifndef LP_PREVIEW_H
#define LP_PREVIEW_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint32_t width,height,frames,frame_bytes,duration_us;
    uint64_t epoch;
} LpPreview;
/* LVP1: 32-byte LE header, then tightly packed I420 frames at uniform PTS.
 * u32 magic, u16 version/header, u16 width/height, u32 frames/frame_us,
 * u32 frame_bytes/payload_bytes/reserved. Header is trusted only after validate. */
int lp_preview_open(const void *,uint64_t,LpPreview *,uint64_t epoch);
int lp_preview_frame(const LpPreview *,uint64_t elapsed_us,uint64_t epoch,uint64_t *offset);
/* Relative path is on the native still's volume; prefix and full folder retained.
 * Same basename RAW+JPEG intentionally share a directory. Volume identity and
 * native-file digest belong in a separately committed manifest. */
int lp_sidecar_path(char out[64],const char folder[9],const char stem[9]);
#endif
