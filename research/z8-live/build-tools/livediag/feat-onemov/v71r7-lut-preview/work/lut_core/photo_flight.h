/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Black-Box Lock-Free Flight Recorder + 2-Second Heartbeat Stall Watchdog (Hardening #4).
 *
 * Adapted from Fenestra-Z8-C311-source/nativepacks/z8_photo_menu/photo_flight.c.
 * Anchored deterministically in verified-free DRAM 0xA11641000..0xA11643FFF:
 *   - 0xA11641000: 64 x 32 B FxZ8FlightRecord ring (2,048 B)
 *   - 0xA11641800: uint64_t atomic head counter (8 B)
 *   - 0xA11641808: 8 x 32 B FxZ8Heartbeat slots (256 B)
 *   - 0xA11641908: 32 B FxZ8WatchdogStatus
 *   - 0xA11641940: Private watchdog scratch + 4 KiB DMA-aligned FXCRASH.LOG bounce buffer
 *
 * Strictly scalar (-mgeneral-regs-only), zero .data/.bss in ROM, zero heap allocation.
 */
#ifndef V71R7_PHOTO_FLIGHT_H
#define V71R7_PHOTO_FLIGHT_H

#include <stddef.h>
#include <stdint.h>
#include "v71r7_lut_abi.h"

#define V71R7_DRAM_FLIGHT_PRIV_VA   UINT64_C(0xA11641940)
#define V71R7_FLIGHT_CRASH_BYTES    4096u
#define V71R7_FLIGHT_STALL_MS       2000u
#define V71R7_FLIGHT_STALL_TICKS    ((uint64_t)V71R7_FLIGHT_STALL_MS * (uint64_t)FX_Z8_TICKS_PER_MS)

/* Direct accessors into the fixed DRAM flight recorder structures */
static inline FxZ8FlightRecord *v71r7_flight_ring_ptr(void) {
    return V71R7_DRAM_PTR(FxZ8FlightRecord, V71R7_DRAM_FLIGHT_RING_VA);
}

static inline uint64_t *v71r7_flight_head_ptr(void) {
    return V71R7_DRAM_PTR(uint64_t, V71R7_DRAM_FLIGHT_HEAD_VA);
}

static inline FxZ8Heartbeat *v71r7_flight_heartbeats_ptr(void) {
    return V71R7_DRAM_PTR(FxZ8Heartbeat, V71R7_DRAM_HEARTBEATS_VA);
}

static inline FxZ8WatchdogStatus *v71r7_flight_watchdog_ptr(void) {
    return V71R7_DRAM_PTR(FxZ8WatchdogStatus, V71R7_DRAM_WATCHDOG_VA);
}

/* Read monotonic 24 MHz counter & 1-based CPU core ID (1..3) */
uint64_t v71r7_flight_ticks(void);
uint16_t v71r7_flight_core(void);

/* Snapshot up to 64 newest flight records in chronological order into out[].
 * Returns number of valid records copied (0..64) and optional lowest seq. */
uint32_t v71r7_flight_snapshot(FxZ8FlightRecord *out_rows, uint32_t max_rows, uint64_t *first_seq_out);

/* Format a complete human-readable FXCRASH report into dst (at least V71R7_FLIGHT_CRASH_BYTES).
 * Returns formatted byte length (0 on overflow/invalid). */
uint32_t v71r7_flight_format_crash(char *dst, uint32_t dst_bytes,
                                   const char *reason, uint64_t now_tick);

/* Write formatted crash dump to <drive>:\FENESTRA\FXCRASH.LOG (.TMP -> .OLD rotation).
 * Returns 0 on success, non-zero error code on failure. */
uint32_t v71r7_flight_write_crash_log(const char *reason, uint64_t now_tick);

#ifdef V71R7_HOST_TEST
/* Injectable host test hooks (stored in DRAM scratch, zero .data/.bss!) */
typedef struct {
    uint64_t (*read_ticks)(void *ctx);
    uint16_t (*read_core)(void *ctx);
    char     (*active_drive)(void *ctx);
    int32_t  (*card_open)(void *ctx, const char *path, uint32_t flags, uint32_t mode);
    uint32_t (*card_write)(void *ctx, int32_t fd, const void *buf, uint32_t bytes, uint32_t *done);
    uint32_t (*card_close)(void *ctx, int32_t fd);
    uint32_t (*card_remove)(void *ctx, const char *path);
    uint32_t (*card_rename)(void *ctx, const char *from, const char *to);
    void    *ctx;
} V71r7FlightHostOps;

void v71r7_flight_set_host_ops(const V71r7FlightHostOps *ops);
#endif

#endif /* V71R7_PHOTO_FLIGHT_H */
