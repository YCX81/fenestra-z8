/* D9 = D8R2 + leaving open gate rewrites the stock LV display object, so leaving is symmetric with entering.
 *
 * Body evidence (D8R2 04.73.d8, 2026-10-28, ../d8r2_body_20261028): after an i-menu cancel from open gate 24p back to
 * ordinary 24p (values unchanged, only the flag), 4 of 6 cancels show one correct 16:9 letterbox frame and then the 16:9
 * picture stretched over the 3:2 screen (r 1.14-1.18), the D5 form; the next rate change restores it.
 *
 * Stock facts (C3.11, this round's README §1):
 *   - every LV snapshot copies the whole display object A03BB751C (0x1F0 bytes) to snapshot +0x198 (40F286A0, under the
 *     object lock A03BB6BD0); the rear-LCD rectangle is object +0x20 (words 3/4 counted from A03BB7530), written only by
 *     the stock display recompute 40F30108 (block 40F32D98-40F32E74: class 40F35F3C from the area, rectangle
 *     411871AC, placement 40F36000 into +0x150; second display 40F32E78-40F32F1C into +0x40/+0x158/+0x15C);
 *   - 40F30108 rewrites those blocks only when its change mask (stored at 40F31B64) has bits 0x5D / 0x6E: the new
 *     display record A03BB74B4 differs from the object, the output type changed, or the area came with the change.
 *     Which applies set them is not under our control (QEMU: first OG30->OG25 yes, 60p->OG24 never, OG24->60p only when
 *     the object was 3:2);
 *   - the area it uses is read at 40F302F8, where the D6 projection (af_local_area_hook) turns area 4 into 0 while the
 *     open-gate flag is 1 and the view kind is set: any stock recompute inside open gate writes the 3:2 placement.
 * Entering open gate, view_commit_hook rewrites the copied rectangle of every snapshot, so the object does not matter.
 * Leaving, it rewrites only the first ordinary snapshot (OG_STATE+0x10 pending) and later snapshots copy the object
 * again: a 3:2 placement left there by a recompute inside open gate is shown until a later recompute happens to run.
 *
 * Change: og_leave_display_hook replaces the pending-clear store `str wzr, [x8, #0x10]` in view_commit_hook (D4).  It
 * performs that store; when the view kind of this snapshot is 0 (that is, the pending restore of the first ordinary
 * snapshot just ran) it also writes both display blocks of the object exactly as the stock recompute does, with the
 * snapshot's native area (+0x35C, the same value the pending restore used; no projection applies with view kind 0).
 * The object's output record (+0x6C) is passed directly instead of the stock copy to A0292A60C, which the callees only
 * read.  No lock is taken (the kernel lock is not known to be recursive): a stock recompute running at the same time
 * computes the same ordinary values, since the flag and the view kind are already 0.  Open-gate snapshots (view kind
 * 1 or 2) are unchanged.  D8R2 is included unchanged and first.
 */
#include "d8_hooks.S"

#define LV_DISPLAY 0xA03BB751C
#define LV_DISPLAY_RECORD 0xA03BB74B4
#define LV_OUTPUT_TYPE1 0xA0292A604
#define LV_OUTPUT_TYPE2 0xA0292A608
#define LV_OUTPUT_FLAG 0xA03BBA348

.text
.balign 16
.global og_leave_display_hook
og_leave_display_hook:
    str wzr, [x8, #0x10]                // replaced instruction: the pending restore is done (x8 = OG_STATE)
    ldr w9, [x8, #4]
    cbnz w9, leave_display_done          // open gate: every snapshot is rewritten, the object is not used
    stp x29, x30, [sp, #-96]!
    stp x20, x21, [sp, #16]
    str x22, [sp, #32]
    ldr x20, =LV_DISPLAY
    ldr w21, [x19, #0x35c]              // native area of this snapshot
    ldr x22, =LV_DISPLAY_RECORD
    /* rear LCD, as 40F32D98-40F32E74; argument block at sp+40 (0x38 bytes) */
    ldp w9, w10, [x20, #0x64]
    stp w9, w10, [sp, #40]              // +0, +4: external output size
    add x9, x20, #0x6c
    str x9, [sp, #48]                   // +8: output record
    add x9, x22, #4
    stp x22, x9, [sp, #56]              // +0x10, +0x18: A03BB74B4, A03BB74B8
    str wzr, [sp, #72]                  // +0x20
    str w21, [sp, #76]                  // +0x24: area
    add x9, x20, #0x18
    add x10, x20, #0x20
    stp x9, x10, [sp, #80]              // +0x28, +0x30
    add x0, sp, #40
    bl original_40F35F3C
    mov w1, w0                          // class
    ldr x9, =LV_OUTPUT_TYPE1
    ldr w9, [x9]
    cmp w9, #0xb
    csel w0, wzr, w9, hi
    ldr x9, =LV_OUTPUT_FLAG
    ldrb w2, [x9]
    add x3, x20, #0x20
    bl original_411871AC                // rectangle, object +0x20
    ldr w9, [x20, #0xc]
    str w9, [x20, #0x14c]
    str wzr, [sp, #76]                  // the stock placement call passes area 0 (40F32E68)
    add x9, x20, #0x150
    str x9, [sp, #88]
    add x0, sp, #40
    bl original_40F36000                // placement, object +0x150
    /* second display, as 40F32E78-40F32F1C */
    str wzr, [sp, #44]
    add x9, x22, #0xc
    add x10, x22, #0x10
    stp x9, x10, [sp, #56]              // A03BB74C0, A03BB74C4
    str w21, [sp, #76]
    add x9, x20, #0x38
    add x10, x20, #0x40
    stp x9, x10, [sp, #80]
    add x0, sp, #40
    bl original_40F35F3C
    mov w1, w0
    ldr x9, =LV_OUTPUT_TYPE2
    ldr w9, [x9]
    mov w10, #4
    cmp w9, #0xb
    csel w21, w10, w9, hi               // output type of the second display (area no longer needed)
    ldr x22, =LV_OUTPUT_FLAG
    ldrb w2, [x22]
    mov w0, w21
    add x3, x20, #0x40
    bl original_411871AC
    ldr w9, [x20, #0x2c]
    str w9, [x20, #0x158]
    mov w0, w21
    mov w1, wzr
    ldrb w2, [x22]
    add x3, x20, #0x15c
    bl original_411871AC
    dsb ishst
    ldr x22, [sp, #32]
    ldp x20, x21, [sp, #16]
    ldp x29, x30, [sp], #96
leave_display_done:
    ret
.ltorg
