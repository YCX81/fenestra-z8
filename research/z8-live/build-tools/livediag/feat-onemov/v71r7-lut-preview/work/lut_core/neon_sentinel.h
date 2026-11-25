/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Runtime NEON/SIMD Context & IRQ Corruption Sentinel (Hardening #2).
 *
 * Adapted from Fenestra-Z8-C311-source/modules/jpeg_lut/neon_sentinel.h.
 * STRICT SCALAR vs NEON SPLIT:
 *   - neon_sentinel.c compiles with -mgeneral-regs-only (0 FP, 0 SIMD, 0 .data/.bss).
 *   - The naked AArch64 EL1 assembly probe macro V71R7_DEFINE_NEON_SENTINEL_ASM
 *     is instantiated ONLY inside cube_frame_neon.c (one of the two NEON TUs).
 */
#ifndef V71R7_NEON_SENTINEL_H
#define V71R7_NEON_SENTINEL_H

#include <stdint.h>
#include "v71r7_lut_abi.h"

/* Reserved 128-byte DRAM slot inside 0xA11640000..0xA11644000 (zero .data/.bss) */
#define V71R7_DRAM_NEON_SENTINEL_VA  UINT64_C(0xA11640080)
_Static_assert(V71R7_DRAM_NEON_SENTINEL_VA >= V71R7_DRAM_ANCHOR_VA + 0x80u &&
               V71R7_DRAM_NEON_SENTINEL_VA + 0x80u <= V71R7_DRAM_FLIGHT_RING_VA,
               "V71R7_DRAM_NEON_SENTINEL_VA must lie cleanly inside DRAM anchor gap");

enum {
    V71R7_NEON_UNAVAILABLE = 0u,
    V71R7_NEON_PASS        = 1u,
    V71R7_NEON_FAILED      = 2u
};

/* Failure detail codes logged with FX_Z8_FLIGHT_NEON_SENTINEL_FAIL */
enum {
    V71R7_NEON_FAIL_PROBE_MISMATCH = 1u,
    V71R7_NEON_FAIL_UNAVAILABLE    = 2u,
    V71R7_NEON_FAIL_INJECTED       = 3u
};

/* Implemented in cube_frame_neon.c (the NEON TU):
 *   - On EL1 AArch64: naked asm verifying d8..d15 across dly_tsk(1) AND
 *     all v0..v31 (64 x 64-bit lanes) across a 3 ms (72,000 tick @ 24 MHz) IRQ window.
 *   - On V71R7_HOST_TEST: userspace AArch64 NEON d8..d15 + v0..v31 canary check. */
unsigned v71r7_neon_sentinel_hw_probe(int32_t (*delay)(uint32_t));

/* Latch V71R7_ANCHOR_FLAG_NEON_DISABLED in v71r7_anchor()->flags and log
 * FX_Z8_FLIGHT_NEON_SENTINEL_FAIL into the black-box flight ring. */
void v71r7_neon_sentinel_latch_disabled(uint32_t reason, uint32_t detail);

/* Query last observed probe status (V71R7_NEON_UNAVAILABLE/PASS/FAILED). */
uint32_t v71r7_neon_sentinel_last_status(void);

#ifdef V71R7_HOST_TEST
typedef unsigned (*V71r7NeonHostProbeFn)(void *ctx, int32_t (*delay)(uint32_t));
void v71r7_neon_sentinel_set_host_probe(V71r7NeonHostProbeFn fn, void *ctx);
void v71r7_neon_sentinel_reset_for_test(void);
#endif

/* ============================================================================
 * Bare-metal AArch64 EL1 Naked Assembly Probe Macro
 * Instantiate ONLY in a NEON-enabled compilation unit (cube_frame_neon.c).
 *
 * TASK_DELAY (0x40187B00) may use caller-saved vectors before the scheduler
 * saves them. First test PCS callee-saved d8..d15 across dly_tsk(1), then test
 * all v0..v31 (512 bytes) across a 3 ms (72,000 ticks @ 24 MHz) IRQ window
 * with zero function calls in the window.
 * ============================================================================ */
#if defined(__aarch64__) && !defined(V71R7_HOST_TEST) && !defined(__APPLE__)

#define V71R7_SIMD_REG_GROUP(op, lo, hi) op " {v" #lo ".16b-v" #hi ".16b}, [x9], #64\n"
#define V71R7_SIMD_ALL_REGS(op) \
    V71R7_SIMD_REG_GROUP(op, 0, 3)   V71R7_SIMD_REG_GROUP(op, 4, 7)  \
    V71R7_SIMD_REG_GROUP(op, 8, 11)  V71R7_SIMD_REG_GROUP(op, 12, 15) \
    V71R7_SIMD_REG_GROUP(op, 16, 19) V71R7_SIMD_REG_GROUP(op, 20, 23) \
    V71R7_SIMD_REG_GROUP(op, 24, 27) V71R7_SIMD_REG_GROUP(op, 28, 31)
#define V71R7_SIMD_D_PAIRS(op, base, offset) \
    op " d8, d9, [" base ", #" #offset "]\n" \
    op " d10, d11, [" base ", #" #offset "+16]\n" \
    op " d12, d13, [" base ", #" #offset "+32]\n" \
    op " d14, d15, [" base ", #" #offset "+48]\n"
#define V71R7_SIMD_VERIFY(lanes) \
    "add x9, sp, #96\n" \
    "adrp x10, .Lv71r7_neon_patterns\n" \
    "add x10, x10, :lo12:.Lv71r7_neon_patterns\n" \
    "mov w11, #" #lanes "\n" \
    "7: ldr x12, [x9], #8\n" \
    "ldr x13, [x10], #8\n" \
    "cmp x12, x13\n" \
    "b.ne 2f\n" \
    "subs w11, w11, #1\n" \
    "b.ne 7b\n"

#define V71R7_DEFINE_NEON_SENTINEL_ASM(fn_name) \
__attribute__((naked)) unsigned fn_name(int32_t (*delay)(uint32_t) __attribute__((unused))) { \
    __asm__ volatile( \
        "mrs x9, cpacr_el1\n" \
        "tbz x9, #20, 5f\n" \
        "mrs x9, daif\n" \
        "tbnz x9, #7, 5f\n" \
        "sub sp, sp, #608\n" \
        "stp x29, x30, [sp]\n" \
        "mov x29, sp\n" \
        V71R7_SIMD_D_PAIRS("stp", "sp", 16) \
        "mrs x9, fpcr\n" \
        "mrs x10, fpsr\n" \
        "stp x9, x10, [sp, #80]\n" \
        "adrp x9, .Lv71r7_neon_patterns\n" \
        "add x9, x9, :lo12:.Lv71r7_neon_patterns\n" \
        V71R7_SIMD_D_PAIRS("ldp", "x9", 0) \
        "mov x9, x0\n" \
        "mov w0, #1\n" \
        "blr x9\n" \
        "mrs x9, cpacr_el1\n" \
        "tbz x9, #20, 4f\n" \
        "cbnz w0, 2f\n" \
        V71R7_SIMD_D_PAIRS("stp", "sp", 96) \
        V71R7_SIMD_VERIFY(8) \
        "adrp x9, .Lv71r7_neon_patterns\n" \
        "add x9, x9, :lo12:.Lv71r7_neon_patterns\n" \
        V71R7_SIMD_ALL_REGS("ld1") \
        "mrs x11, cntpct_el0\n" \
        "movz w12, #0x1940\n" \
        "movk w12, #1, lsl #16\n" /* 72,000 ticks @ 24 MHz = 3 ms */ \
        "1: mrs x13, cntpct_el0\n" \
        "sub x13, x13, x11\n" \
        "cmp x13, x12\n" \
        "b.lo 1b\n" \
        "mrs x9, cpacr_el1\n" \
        "tbz x9, #20, 4f\n" \
        "add x9, sp, #96\n" \
        V71R7_SIMD_ALL_REGS("st1") \
        V71R7_SIMD_VERIFY(64) \
        "mov w0, #1\n" \
        "b 3f\n" \
        "2: mov w0, #2\n" \
        "3:\n" \
        V71R7_SIMD_D_PAIRS("ldp", "sp", 16) \
        "ldp x9, x10, [sp, #80]\n" \
        "msr fpcr, x9\n" \
        "msr fpsr, x10\n" \
        "b 6f\n" \
        "4: mov w0, #2\n" \
        "mrs x11, daif\n" \
        "msr daifset, #15\n" \
        "orr x10, x9, #0x100000\n" \
        "msr cpacr_el1, x10\n" \
        "isb\n" \
        V71R7_SIMD_D_PAIRS("ldp", "sp", 16) \
        "ldp x12, x13, [sp, #80]\n" \
        "msr fpcr, x12\n" \
        "msr fpsr, x13\n" \
        "msr cpacr_el1, x9\n" \
        "isb\n" \
        "msr daif, x11\n" \
        "6: ldp x29, x30, [sp]\n" \
        "add sp, sp, #608\n" \
        "ret\n" \
        "5: mov w0, #0\n" \
        "ret\n" \
        ".pushsection .rodata.v71r7_neon_patterns, \"a\", @progbits\n" \
        ".balign 16\n" \
        ".Lv71r7_neon_patterns:\n" \
        ".set v71r7_neon_lane, 0\n" \
        ".rept 64\n" \
        ".quad 0x5eb19c73d284a60f ^ (v71r7_neon_lane * 0x9e3779b97f4a7c15)\n" \
        ".set v71r7_neon_lane, (v71r7_neon_lane + 1)\n" \
        ".endr\n" \
        ".popsection\n" \
    ); \
}

#endif /* __aarch64__ && !V71R7_HOST_TEST && !__APPLE__ */

#endif /* V71R7_NEON_SENTINEL_H */
