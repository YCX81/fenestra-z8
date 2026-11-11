/* Emulation-only preview kernel. No GUI, filesystem or native task ownership. */
#ifndef LP_PREVIEW_H
#define LP_PREVIEW_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint32_t width,height,frames,frame_bytes,duration_us;
    uint64_t epoch;
} LpPreview;
/* Legacy LVP1 (v36 and older shots): 32-byte LE header, then tightly packed
 * I420 frames at uniform PTS. Header is trusted only after validate. */
int lp_preview_open(const void *,uint64_t,LpPreview *,uint64_t epoch);
/* Frame index for the elapsed time; 1 at the end of the clip, -1 when stale. */
int lp_preview_index(const LpPreview *,uint64_t elapsed_us,uint64_t epoch,uint32_t *index);

/* v37: preview pictures inside the Live MOV, in a trailing free atom:
 *   u32be size,"free","LVJ1",u16 version=1,u16 header,u16 w,u16 h,u32 frames,
 *   u32 frame_us,u32 0,u32 size[frames], JFIF pictures, u32 size,"LVJT".
 * All little endian but the atom size. See the writer for the exact contract. */
#define LP_CLIP_FRAMES 90u
#define LP_CLIP_HEAD (8u+24u+4u*LP_CLIP_FRAMES)
#define LP_JPEG_MAX (192u*1024u)
typedef struct { uint64_t at[LP_CLIP_FRAMES+1u],atom,end; } LpClip;   /* file offsets, at[frames] = end of pictures */
/* Optional sound record right after the pictures: "LVJA", pcm offset, frames, 48000. */
int lp_clip_audio(const uint8_t record[16],const LpClip *,uint32_t *offset,uint32_t *frames);
/* tail: the last 8 bytes of the file. 0 and the atom offset/size on success. */
int lp_clip_tail(const uint8_t tail[8],uint64_t file_bytes,uint64_t *atom,uint32_t *atom_bytes);
/* head: the first min(atom_bytes, LP_CLIP_HEAD) bytes of the atom. */
int lp_clip_open(const uint8_t *head,size_t head_bytes,uint64_t atom,uint32_t atom_bytes,
                 LpPreview *,LpClip *,uint64_t epoch);

/* Baseline JPEG, 4:2:0, three components, no restart markers: what the Live
 * writer emits. Output is tightly packed I420 of width x height. */
typedef struct { uint16_t look[512];int32_t maxcode[18];uint16_t first[17],offset[17];uint8_t vals[256]; } LpHuffman;
typedef struct {
    LpHuffman table[2][2];           /* [class][id] */
    uint16_t q[2][64];
    const uint8_t *p,*end;
    uint32_t acc;int bits;
} LpJpeg;
int lp_jpeg_decode(LpJpeg *,const uint8_t *data,size_t bytes,uint8_t *i420,uint32_t width,uint32_t height);
#endif
