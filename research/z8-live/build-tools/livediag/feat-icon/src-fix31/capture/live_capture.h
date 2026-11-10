#ifndef LIVE_A_CAPTURE_H
#define LIVE_A_CAPTURE_H
#include <stdint.h>
#include <stddef.h>
#ifndef LA_WIDTH
#define LA_WIDTH 640u
#endif
#ifndef LA_HEIGHT
#define LA_HEIGHT 424u
#endif
#define LA_FRAME_BYTES (LA_WIDTH * LA_HEIGHT * 3u / 2u)
#define LA_CAPACITY 96u
#ifndef LA_RING_CAPACITY
#define LA_RING_CAPACITY LA_CAPACITY
#endif
#ifndef LA_SAMPLE_INTERVAL_US
#define LA_SAMPLE_INTERVAL_US 25000u
#endif
#ifndef LA_GAP_LIMIT_US
#define LA_GAP_LIMIT_US 100000u
#endif
#define LA_RING_BYTES (LA_FRAME_BYTES * LA_RING_CAPACITY)
#define LA_WINDOW_US UINT64_C(1500000)
#define LA_MAX_GAP_US ((uint64_t)LA_GAP_LIMIT_US)
enum la_phase { LA_OFF, LA_REQUESTED, LA_READY, LA_POST, LA_FROZEN, LA_ENCODING, LA_FAILED };
enum la_reason { LA_NONE, LA_ALLOC, LA_SOURCE_UNPROVEN, LA_FORMAT, LA_GAP, LA_CANCEL, LA_OVERFLOW };
/* The callback interface also supports a strict lease; the active v2 adapter
 * is best effort and does not prove exclusion of concurrent DMA. It checks
 * the selected source, completion sequence and epoch before publishing. */
struct la_source {
    const uint8_t *y, *uv;
    uint32_t width, height, y_stride, uv_stride;
    uint64_t generation, timestamp_us, lease;
};
struct la_ops {
    void *context;
    int (*allocate)(void *, uint32_t, void **descriptor, uint8_t **data, uint32_t *actual);
    void (*release)(void *, void *descriptor);
    int (*lease_begin)(void *, uint64_t generation, uint64_t timestamp, struct la_source *);
    int (*lease_valid)(void *, const struct la_source *);
    void (*lease_end)(void *, const struct la_source *);
};
struct la_frame { uint64_t timestamp_us, generation; };
struct la_capture {
    struct la_ops ops;
    void *descriptor;
    uint8_t *pixels;
    struct la_frame frames[LA_CAPACITY];
    uint32_t actual_bytes, head, count, phase, reason;
    uint64_t copied, dropped, rejected_presses, accepted_presses;
    uint64_t last_generation, stream_start, press_us;
    /* Atomic mailboxes only; worker owns everything above except phase reads. */
    uint64_t irq_seq, irq_timestamp, press_mailbox;
    uint32_t requested, cancel_epoch, worker_epoch, reader_active;
};
void la_init(struct la_capture *, const struct la_ops *);
void la_request(struct la_capture *, int enabled);
void la_cancel(struct la_capture *); /* mode/sleep/card change: immediate invalidation */
void la_irq_signal(struct la_capture *, uint32_t event, uint64_t status, uint64_t timestamp_us);
int la_full_press(struct la_capture *, uint64_t timestamp_us);
/* Sole capture worker calls step, begin_encode, finish_encode. flags=1 only
 * after PHOTO, ordinary selector0, not HSFC/pre-release/C30/C60, active card
 * and LV/sleep state have been checked by the native integration. */
void la_step(struct la_capture *, uint64_t now_us, int eligible);
int la_begin_encode(struct la_capture *);
const uint8_t *la_frame_at(const struct la_capture *, uint32_t index, struct la_frame *);
void la_finish_encode(struct la_capture *);
int la_cancelled(const struct la_capture *);
int la_nv12_to_i420(uint8_t *, const struct la_source *);
#endif
