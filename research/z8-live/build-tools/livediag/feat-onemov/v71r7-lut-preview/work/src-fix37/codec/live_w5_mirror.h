#ifndef LIVE_W5_MIRROR_H
#define LIVE_W5_MIRROR_H
#include <stddef.h>
#include <stdint.h>

/* V70 cached mirror of Wave5 pool ownership, close state, and Opt A/B direct-NV12
 * + 1-deep async pipelined state. Occupies bytes 0..255 of struct live_runtime.lv_regs[228]
 * (912 B total; bytes 256..767 belong exclusively to struct live_flip_obs).
 *
 * First 56 bytes are byte-for-byte identical to V69 struct live_w5_mirror so all V69
 * HangFix invariants and offsets remain preserved with zero ABI change. */
#define LIVE_W5_MIRROR_V70 1

struct live_w5_mirror_v70 {
    /* ---- V69 HangFix fields (bytes 0..55, offsets pinned) ---- */
    uint32_t magic;
    uint32_t stream;
    uint32_t busy;
    uint32_t release_req;
    uint32_t close_failed;
    uint32_t handoffs;
    uint64_t arena;
    uint64_t native_instance;
    void    *arena_desc;
    void    *block_desc;

    /* ---- V70 Opt A (Direct NV12 / 0-copy CFR repeat) + Opt B (1-deep async) ---- */
    uint32_t in_flight;          /* 1 while 1 Wave5 picture is triggered and unharvested */
    uint32_t in_flight_slot;     /* NV12 slot (0..3) currently owned by Wave5 DMA */
    uint32_t in_flight_hw_idx;   /* hw_index of in-flight picture (INFO slot = hw_idx % 9) */
    uint32_t in_flight_stamp;    /* expected 90 kHz stamp = hw_idx * 3000u */
    int32_t  last_valid_slot;    /* last committed & cleaned NV12 slot (0..3), or -1 */
    uint32_t slot_valid_mask;    /* bitmask (bits 0..3) of slots with valid cleaned NV12 */
    uint32_t next_slot_rr;       /* round-robin cursor (0..3) for picking next free slot */
    uint32_t batch_submitted;    /* 1 after manual-mode SUBMIT following RUN() */
    uint32_t submitted_frames;   /* total pictures submitted since open_stream / close */
    uint32_t in_flight_retrig;   /* 1 if in-flight frame is a 0-copy/0-cvac CFR repeat */
    uint32_t direct_nv12_cnt;    /* total fresh direct-NV12 frames submitted */
    uint32_t async_retrig_cnt;   /* total 0-copy/0-cvac CFR repeat re-triggers */
    uint64_t in_flight_pts;      /* presentation timestamp (us) of in-flight picture */
    uint64_t in_flight_t0_us;    /* now_us() immediately after TRIGGER (for timeout) */
    uint64_t *in_flight_idr_ptr; /* caller's &s->last_idr_us, updated on harvest if IRAP */

    /* ---- V70 Opt C cached timing accumulators for EV_DIAG3 (POOL_GATE-immune) ---- */
    uint64_t sum_in_us;
    uint64_t sum_wait_us;
    uint64_t sum_out_us;
    uint64_t sum_parse_us;
    uint32_t n_timed_v70;
    uint32_t last_poll_wait_us;

    /* ---- V71: -24 Quiet-Park + POOL_GATE-immune close diagnostics ---- */
    uint32_t park_err;           /* 24 after -24 timeout while alive; cleared on late-INFO self-heal or close */
    uint32_t diag_close;         /* low16: eos_miss, bits 16..23: W5_FINISH WSTATE, bits 24..31: close-fail stage */
};

_Static_assert(sizeof(struct live_w5_mirror_v70) <= 256u, "w5_mirror_v70 must fit in lv_regs[0..255]");
_Static_assert(offsetof(struct live_w5_mirror_v70, magic) == 0u, "magic offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, stream) == 4u, "stream offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, busy) == 8u, "busy offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, release_req) == 12u, "release_req offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, close_failed) == 16u, "close_failed offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, handoffs) == 20u, "handoffs offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, arena) == 24u, "arena offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, native_instance) == 32u, "native_instance offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, arena_desc) == 40u, "arena_desc offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, block_desc) == 48u, "block_desc offset");
_Static_assert(offsetof(struct live_w5_mirror_v70, in_flight) == 56u, "v70 fields follow v69 prefix");

#endif
