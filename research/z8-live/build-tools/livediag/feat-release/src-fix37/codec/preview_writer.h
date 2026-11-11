#ifndef LIVE_PREVIEW_WRITER_H
#define LIVE_PREVIEW_WRITER_H
#include <stddef.h>
#include <stdint.h>

#define LIVE_PREVIEW_WIDTH 1024u
#define LIVE_PREVIEW_HEIGHT 682u
#define LIVE_PREVIEW_FRAMES 45u
#define LIVE_PREVIEW_FRAME_US 66667u
#define LIVE_PREVIEW_FRAME_BYTES (1024u * 682u * 3u / 2u)
/* Largest compressed frame the in-camera player accepts (its read buffer). */
#define LIVE_PREVIEW_JPEG_MAX (192u * 1024u)
/* A picture above this makes the following ones one quality step coarser. */
#define LIVE_PREVIEW_JPEG_SOFT (72u * 1024u)
#define LIVE_PREVIEW_HEADER (24u + 4u * LIVE_PREVIEW_FRAMES)

/* v37: the in-camera preview lives inside the Live MOV itself, in one trailing
 * QuickTime 'free' atom after moov (every MOV reader skips it), instead of the
 * separate 47 MB LIVEDATA/.LVP file:
 *
 *   u32be atom size, 'free'
 *   'LVJ1', u16le version=1, u16le header bytes (24 + 4*frames),
 *   u16le width, u16le height, u32le frames, u32le frame_us, u32le 0,
 *   u32le size[frames]
 *   frames: complete baseline JFIF pictures (4:2:0), back to back
 *   optional 'LVJA', u32le pcm offset, u32le pcm frames, u32le 48000:
 *     where the MOV's own 48 kHz stereo s16le track sits in mdat (preview sound)
 *   u32le atom size, 'LVJT'            <- last 8 bytes of the file
 *
 * A reader takes the last 8 bytes, steps back by the atom size and checks the
 * header. Every frame is at most LIVE_PREVIEW_JPEG_MAX bytes.
 * write_at must complete the whole write or return nonzero. On any error the
 * caller removes the file: a MOV with a partial atom is never published. */
typedef struct {
    void *ctx;
    int (*frame)(void *ctx, uint32_t index, void *i420, size_t bytes);
    int (*write_at)(void *ctx, uint64_t at, const void *data, size_t bytes);
    void (*yield)(void *ctx);
} LivePreviewWriter;
/* scratch: one I420 frame. io: staging buffer, at least 4096 bytes.
 * base: file offset of the atom (the current end of the MOV). *end: new file size. */
int live_preview_write(const LivePreviewWriter *, void *scratch, size_t scratch_bytes,
                       uint8_t *io, size_t io_bytes, uint64_t base, uint64_t *end,
                       uint32_t audio_offset, uint32_t audio_frames);
/* One picture; exposed for host tests. Returns 0 and the byte count, -1 on a
 * write error, 1 when the picture would exceed max_bytes. */
int live_preview_jpeg(const LivePreviewWriter *, const uint8_t *i420, unsigned width, unsigned height,
                      unsigned quality, uint8_t *io, size_t io_bytes, uint64_t at, uint32_t max_bytes,
                      uint32_t *bytes);
#endif
