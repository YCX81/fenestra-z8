#ifndef LIVE_PREVIEW_WRITER_H
#define LIVE_PREVIEW_WRITER_H
#include <stddef.h>
#include <stdint.h>

#define LIVE_PREVIEW_WIDTH 1024u
#define LIVE_PREVIEW_HEIGHT 682u
#define LIVE_PREVIEW_FRAMES 45u
#define LIVE_PREVIEW_FRAME_US 66667u
#define LIVE_PREVIEW_FRAME_BYTES (1024u * 682u * 3u / 2u)
#define LIVE_PREVIEW_BYTES (32u + LIVE_PREVIEW_FRAMES * LIVE_PREVIEW_FRAME_BYTES)

/* LVP1, little endian, no padding: magic u32=0x3150564c; version u16=1;
 * header bytes u16=32; width/height u16; frames, frame_us, frame_bytes,
 * payload_bytes, reserved(0) u32. Then 45 tightly packed I420 frames.
 * The writer calls frame/read and write in the caller's worker context.
 * The caller owns an exclusively created temporary file and must remove it
 * after any error, close successfully, then publish it before MANIFEST.
 * No partial output is considered a valid PREV.LVP.
 */
typedef struct {
    void *ctx;
    int (*frame)(void *ctx, uint32_t index, void *i420, size_t bytes);
    int (*write)(void *ctx, const void *data, size_t bytes, size_t *written);
    void (*yield)(void *ctx);
} LivePreviewWriter;
int live_preview_write(const LivePreviewWriter *, void *scratch, size_t scratch_bytes);
#endif
