/* D8 = D7 + after an i-menu cancel that changed the open-gate flag, the LV controller also treats a change of the
 * open-gate geometry as a change of the LV state (direction c of the cancel-H1 round, ../cancel_h1_20261027/README.md §5).
 * This file builds D8R2 04.73.d8 (armed raise, compare hook in the stock frame, no extra stack in the LV task); 04.71.d8
 * (commit 19162c2b, raise without arming) and 04.72.d8 (commit c02833dd, armed, 32-byte compare wrapper) failed their
 * offline gates, see README §5.
 *
 * Body evidence (D6 and D7, 2026-10-27): an i-menu cancel whose rollback keeps 125A/125B/125C and only changes the
 * open-gate flag (ordinary 24p -> ... -> open gate 24p -> cancel) leaves the sensor in the open-gate readout while the
 * display follows the flag; LV then stops following the selection and power-off is delayed.  QEMU (cancel-H1 round):
 * after such a rollback the stock mode setup (4115FC3C, called from the sequence task's bit-7 block at 40F55580) does
 * not run; after every value-changing apply or rollback it does.
 *
 * Stock chain (C3.11, this round's README §1): the LV controller builds a new LV state (40F79348; the D-series
 * configuration_hook at 40F7938C already writes the open-gate readout geometry there, from the flag and the state),
 * compares it with the current state field by field (40F81010: kind, readout, size, rate, ... -- the flag is not a
 * field) and only when they differ runs the applier 40F816B0, whose mode branch calls 41034FA4 -> 40F4E108 ->
 * 40F5BFD8, which posts bit 7 to the sequence task A03BBB4E0.  With equal fields the geometry tables change but the
 * sensor mode is never set up again.
 *
 * Change:
 *   og_apply_hook     replaces bl 40F816B0 at both applier calls (40F7BF9C, 40F7CC08): records in OG_APPLIED (OG_STATE
 *                     +0x120, bit 8 valid, bit 0 open gate) whether the state being applied selects the open-gate
 *                     geometry, then tail-calls the applier
 *   ds_session_end_d8_hook  replaces bl ds_session_end_hook (D7) at the four 410D8400 call sites: after D7's hook
 *                     returns, a rollback (w0 = 0) that changed the open-gate flag arms OG_APPLIED (bit 9) when a state
 *                     was applied since boot.  Only the i-menu cancel arms; the main-menu same-rate commit does not
 *                     (D8 first build 04.71.d8 raised there too, inside the menu, and the QEMU i-menu regression then
 *                     failed at the next MENU press; stock D7 sets the mode up when the menu is left)
 *   og_compare_exit_hook  replaces the first epilogue instruction of the stock compare 40F81010 (40F815F4), so it acts at
 *                     all three compare calls (40F7BF58 event path, 40F7C878 / 40F7C978 in the ~33 ms LV loop; QEMU D7
 *                     control: a value-changing apply makes both loop compares return 1 and the applier follows from
 *                     40F7CC08) inside the stock frame, without adding stack (D8R 04.72.d8 wrapped the three calls with a
 *                     32-byte frame; every D8 build so far failed the QEMU i-menu regression in the main-menu round
 *                     trip while D7 passed): when the stock compare finds no difference, a state was
 *                     applied since boot, OG_APPLIED is armed, and
 *                     the open-gate geometry of the new state differs from the applied one, report a difference
 *                     (reason bit 31, otherwise unused; the applier only logs the reason word and passes bit 12 on),
 *                     unless the stock compare's own final overrides would force "no difference" (A03BC22D4 == 1 with
 *                     new kind 1 and new +4 == 1, or A03BC2B84 == 5), which are checked the same way here; armed with
 *                     nothing to do (same geometry) disarms.  The applier hook rewrites OG_APPLIED, which disarms too.
 *                     Arming and disarming are exclusive read-modify-writes (the applier writes from the LV task, the
 *                     session end runs in the UI task)
 *   og_gate_wanted    the open-gate predicate of configuration_hook (flag 1, kind 1, readout 4, size 6, rate 4..6,
 *                     request kind 4, observed file type 6) evaluated on a given LV state; leaf, x9/x10 only
 *   boot_reset_d8_hook  41184DE8 (both boot paths, w19 == 1 after the text mapping): clears OG_APPLIED, then the D7
 *                     hook (Q3 stash, then the D4 reset of OG_STATE+0..0x120).  OG_APPLIED lies outside the D4 range.
 * The D7 hooks are included unchanged and first, so every D7 address stays the same; the new code follows them.
 */
#include "d7_hooks.S"

#define OG_APPLIED (OG_STATE + 0x120)
#define APPLIED_VALID 8
#define APPLIED_ARMED 9
#define LV_REASON 0xA03BC4330

.text
.balign 16
.global og_gate_wanted
og_gate_wanted:
    ldr x9, =0xA03D5C02B
    ldrb w9, [x9]
    cmp w9, #1
    b.ne 8f
    ldr w9, [x0]
    cmp w9, #1
    b.ne 8f
    ldr w9, [x0, #0x388]
    cmp w9, #4
    b.ne 8f
    ldr w9, [x0, #0x38c]
    cmp w9, #6
    b.ne 8f
    ldr w9, [x0, #0x390]
    sub w9, w9, #4
    cmp w9, #2
    b.hi 8f
    ldr w9, [x0, #0x3a0]
    cmp w9, #4
    b.ne 8f
    ldr x10, =0xA03D5C014
    ldr w9, [x10]
    cmp w9, #6
    b.ne 8f
    mov w0, #1
    ret
8:  mov w0, wzr
    ret

/* x0 = LV state about to be applied; replaces bl 40F816B0.  No stack: x16/x17 (free at a call) keep x0 and x30. */
.balign 16
.global og_apply_hook
og_apply_hook:
    mov x16, x0
    mov x17, x30
    bl og_gate_wanted
    orr w0, w0, #(1 << APPLIED_VALID)
    ldr x9, =OG_APPLIED
    str w0, [x9]
    mov x0, x16
    mov x30, x17
    b original_40F816B0

/* Replaces the first epilogue instruction of the stock compare 40F81010 (40F815F4 ldp x19, x30, [sp, #0x50]), which
 * every return path of that function reaches; runs inside the stock frame and adds no stack.  In: w0 = stock result,
 * x19 = new LV state (40F81028 mov x19, x1).  x30 is reloaded by the epilogue, so the leaf call may use it. */
.balign 16
.global og_compare_exit_hook
og_compare_exit_hook:
    tbnz w0, #0, 9f                    // stock difference: the applier records the geometry
    ldr x9, =OG_APPLIED
    ldr w11, [x9]
    tbz w11, #APPLIED_VALID, 9f        // nothing applied since this boot: stock decision
    tbz w11, #APPLIED_ARMED, 9f        // only after an i-menu cancel changed the flag
    mov x0, x19
    bl og_gate_wanted                  // x9, x10 only
    and w10, w11, #1
    cmp w0, w10
    mov w0, wzr                        // the stock result again
    b.ne 1f
    ldr x9, =OG_APPLIED                // armed, same geometry: disarm
3:  ldxr w10, [x9]
    and w10, w10, #~(1 << APPLIED_ARMED)
    stxr w11, w10, [x9]
    cbnz w11, 3b
    b 9f
1:  ldr x9, =0xA03BC22D4               // stock final override 1
    ldr w9, [x9]
    cmp w9, #1
    b.ne 2f
    ldr w9, [x19]
    cmp w9, #1
    b.ne 2f
    ldr w9, [x19, #4]
    cmp w9, #1
    b.eq 9f
2:  ldr x9, =0xA03BC2B84               // stock final override 2
    ldr w9, [x9]
    cmp w9, #5
    b.eq 9f
    ldr x9, =LV_REASON
    ldr w10, [x9]
    orr w10, w10, #0x80000000
    str w10, [x9]
    mov w0, #1
9:  ldp x19, x30, [sp, #0x50]          // replaced instruction
    b original_40F815F8

/* w0 = 1 commit / 0 rollback; replaces bl ds_session_end_hook at the four 410D8400 call sites; returns its result.
 * One 16-byte frame: w19 = flag before | commit << 8. */
.balign 16
.global ds_session_end_d8_hook
ds_session_end_d8_hook:
    stp x19, x30, [sp, #-16]!
    ldr x9, =OG_FLAG
    ldrb w19, [x9]                     // flag before (D7 writes the entry flag back on a rollback)
    orr w19, w19, w0, lsl #8
    bl ds_session_end_hook
    tbnz w19, #8, 9f                   // commit: never arms
    ldr x9, =OG_FLAG
    ldrb w10, [x9]
    and w11, w19, #0xff
    cmp w10, w11
    b.eq 9f
    ldr x9, =OG_APPLIED
1:  ldxr w10, [x9]
    tbz w10, #APPLIED_VALID, 8f
    orr w10, w10, #(1 << APPLIED_ARMED)
    stxr w11, w10, [x9]
    cbnz w11, 1b
    b 9f
8:  clrex
9:  ldp x19, x30, [sp], #16
    ret

.balign 16
.global boot_reset_d8_hook
boot_reset_d8_hook:
    cmp w19, #1
    b.ne 1f
    stp x8, x9, [sp, #-16]!
    ldr x8, =OG_APPLIED
    str wzr, [x8]
    dsb ishst
    ldp x8, x9, [sp], #16
1:  b boot_reset_hook
.ltorg
