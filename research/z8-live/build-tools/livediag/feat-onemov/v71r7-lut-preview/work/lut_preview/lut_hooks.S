/* SPDX-License-Identifier: GPL-3.0-only */
/* V71.7 Display Layer 0 & Power Sync AArch64 EL1 Hook Entry Stubs.
 *
 * Verified-clean stock Z8 C3.11 BL call sites (all 6 confirmed 100% stock in V71.6 final/main.main):
 *   1. 0x40E9A540: BL 0x40CD7AD8 (preimage 0x97f8f566 -> 66 f5 f8 97) -> v71r7_layer0_enqueue_hook
 *   2. 0x40CD862C: BL 0x402B6D88 (preimage 0x97d779d7 -> d7 79 d7 97) -> v71r7_layer0_display_hook
 *   3. 0x40CD72A4: BL 0x40CD9640 (preimage 0x940008e7 -> e7 08 00 94) -> v71r7_layer0_vsync_hook (LCD)
 *   4. 0x40CD72D4: BL 0x40CD9640 (preimage 0x940008db -> db 08 00 94) -> v71r7_layer0_vsync_hook (EVF)
 *   5. 0x40D67328: BL 0x40D60E94 (preimage 0x97ffe6db -> db e6 ff 97) -> v71r7_power_sync_hook (w0=11 POWER_CANCEL_SYNC)
 *   6. 0x40D67340: BL 0x40D60E94 (preimage 0x97ffe6d5 -> d5 e6 ff 97) -> v71r7_power_sync_hook (w0=10 POWER_OFF_SYNC)
 *
 * All 6 sites are standard AAPCS64 BL calls; these thin stubs preserve registers/ABI with
 * 0 FP/SIMD instructions and 0 writable .data/.bss bytes.
 */

.text
.align 2

/* --- 4 Hook Entry Points (Stage 1 BL targets) --- */

.global v71r7_layer0_enqueue_hook
.type v71r7_layer0_enqueue_hook, %function
v71r7_layer0_enqueue_hook:
    b v71r7_layer0_enqueue_hook_c
.size v71r7_layer0_enqueue_hook, .-v71r7_layer0_enqueue_hook

.global v71r7_layer0_display_hook
.type v71r7_layer0_display_hook, %function
v71r7_layer0_display_hook:
    b v71r7_layer0_display_hook_c
.size v71r7_layer0_display_hook, .-v71r7_layer0_display_hook

.global v71r7_layer0_vsync_hook
.type v71r7_layer0_vsync_hook, %function
v71r7_layer0_vsync_hook:
    b v71r7_layer0_vsync_hook_c
.size v71r7_layer0_vsync_hook, .-v71r7_layer0_vsync_hook

.global v71r7_power_sync_hook
.type v71r7_power_sync_hook, %function
v71r7_power_sync_hook:
    b v71r7_power_sync_hook_c
.size v71r7_power_sync_hook, .-v71r7_power_sync_hook

/* --- 4 Direct Stock ROM Tail-Call Adapters (zero literal pools in .text) --- */

.global v71r7_stock_layer0_enqueue
.type v71r7_stock_layer0_enqueue, %function
v71r7_stock_layer0_enqueue:
    movz x16, #0x7ad8
    movk x16, #0x40cd, lsl #16
    br   x16
.size v71r7_stock_layer0_enqueue, .-v71r7_stock_layer0_enqueue

.global v71r7_stock_layer0_display
.type v71r7_stock_layer0_display, %function
v71r7_stock_layer0_display:
    movz x16, #0x6d88
    movk x16, #0x402b, lsl #16
    br   x16
.size v71r7_stock_layer0_display, .-v71r7_stock_layer0_display

.global v71r7_stock_layer0_vsync
.type v71r7_stock_layer0_vsync, %function
v71r7_stock_layer0_vsync:
    movz x16, #0x9640
    movk x16, #0x40cd, lsl #16
    br   x16
.size v71r7_stock_layer0_vsync, .-v71r7_stock_layer0_vsync

.global v71r7_stock_power_sync
.type v71r7_stock_power_sync, %function
v71r7_stock_power_sync:
    movz x16, #0x0e94
    movk x16, #0x40d6, lsl #16
    br   x16
.size v71r7_stock_power_sync, .-v71r7_stock_power_sync

.section .note.GNU-stack,"",%progbits
