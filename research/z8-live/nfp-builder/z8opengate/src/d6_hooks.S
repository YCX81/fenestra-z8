/* D6 = D5 + every open-gate override that keys on the latched view kind also requires the open-gate flag.
 *
 * Body evidence (D5, 2026-10-27, d5_body_20261027): moving the i-menu cursor from an open-gate row to
 * `[FX] 8256x4644` shows 2 letterbox frames and then the 16:9 band stretched over the 3:2 screen (4 of 4 in the
 * video) until a mode with another sensor readout is selected.
 *
 * QEMU trace of the shipped D5 bytes (d6-trace-5, same transition), per event, virtual time from the apply:
 *   +0.00 ms  i-menu apply; the menu transaction has already written the open-gate flag (A03D5C02B) = 0
 *   +0.14 ms  af_local_area_hook (native 40F302F8, local area read of the display/AF control): the latched view kind
 *             (OG_STATE+4) is still 1, so project_af_register turns the video area 4 into 0
 *   +1.69 ms  native display recompute (40F32E34/40F32E74) reads that area 0 -> class 0 (3:2) and writes it into the
 *             display control object A03BB751C; nothing recomputes it later because for the stock code the area never changed
 *   +28 ms    first request-0 snapshot commit: view kind := 0 and the pending restore writes the ordinary rectangle into
 *             that ONE snapshot (= the two letterbox frames); later snapshots take the rectangle from the stale object
 * A first attempt that only gated display_prepare did not change this transition at all (d6-trace-2): the area was
 * already 0 when display_prepare saw it.  Both places need the rule.
 *
 * Change: project_af_register, project_af_class (AF local area / class projections) and display_prepare,
 * display_size_hook (display class / placement / local buffer overrides) additionally require the open-gate flag to be 1.
 * In every open-gate state the flag is 1, so those paths are unchanged; after a menu apply that leaves open-gate the
 * stale view kind no longer applies them.  Nothing else differs from D5.
 */
#define project_af_register project_af_register_d5
#define project_af_class project_af_class_d5
#define af_local_area_hook af_local_area_hook_d5
#define af_grid_hook af_grid_hook_d5
#define af_custom_grid_hook af_custom_grid_hook_d5
#define af_class1_hook af_class1_hook_d5
#define af_class2_hook af_class2_hook_d5
#define display_prepare display_prepare_d5
#define display_class_hook display_class_hook_d5
#define display_class_stock display_class_stock_d5
#define display_place_hook display_place_hook_d5
#define display_place_stock display_place_stock_d5
#define display_size_hook display_size_hook_d5
#define display_size_stock display_size_stock_d5
#define local_display_size local_display_size_d5
#include "d5_hooks.S"
#undef project_af_register
#undef project_af_class
#undef af_local_area_hook
#undef af_grid_hook
#undef af_custom_grid_hook
#undef af_class1_hook
#undef af_class2_hook
#undef display_prepare
#undef display_class_hook
#undef display_class_stock
#undef display_place_hook
#undef display_place_stock
#undef display_size_hook
#undef display_size_stock
#undef local_display_size

.macro project_af_register reg
    stp x16, x17, [sp, #-32]!
    mrs x16, nzcv
    str x16, [sp, #16]
    cmp \reg, #4
    b.ne 1f
    ldr x17, =OG_FLAG
    ldrb w17, [x17]
    cmp w17, #1
    b.ne 1f                         // selection is not open-gate: a latched view kind is stale
    ldr x17, =OG_STATE
    ldr w17, [x17, #4]
    cbnz w17, 2f
    require_selected_video 1f
2:  mov \reg, #0
1:
    ldr x16, [sp, #16]
    msr nzcv, x16
    ldp x16, x17, [sp], #32
.endm
.macro project_af_class info
    stp x16, x17, [sp, #-32]!
    mrs x16, nzcv
    str x16, [sp, #16]
    ldr w17, [\info, #0x388]
    cmp w17, #4
    b.ne 1f
    ldr x17, =OG_FLAG
    ldrb w17, [x17]
    cmp w17, #1
    b.ne 1f
    ldr x17, =OG_STATE
    ldr w17, [x17, #4]
    cbnz w17, 2f
    require_selected_video 1f
2:  mov w8, #0
1:
    ldr x16, [sp, #16]
    msr nzcv, x16
    ldp x16, x17, [sp], #32
.endm
.balign 16
.global af_local_area_hook
af_local_area_hook:
    project_af_register w8
    str w8, [sp, #0x88]
    b original_40F302FC
.balign 16
.global af_grid_hook
af_grid_hook:
    ldp w8, w10, [x20, #0xc]
    project_af_register w8
    b original_40F34A4C
.balign 16
.global af_custom_grid_hook
af_custom_grid_hook:
    ldp w9, w10, [x20, #0xc]
    project_af_register w9
    b original_40F349C0
.balign 16
.global af_class1_hook
af_class1_hook:
    project_af_class x19
    str w8, [x19, #0x3c0]
    b original_40F28928
.balign 16
.global af_class2_hook
af_class2_hook:
    project_af_class x21
    str w8, [x21, #0x3c0]
    b original_40F350EC

.macro display_prepare fallback
    ldr x8, =OG_STATE
    ldr w8, [x8, #4]
    cbz w8, \fallback
    ldr x8, =OG_FLAG
    ldrb w8, [x8]
    cmp w8, #1
    b.ne \fallback                  // selection already left open-gate: the latched view is stale
    ldp w8, w9, [x0]
    orr w8, w8, w9
    cbnz w8, \fallback
    ldr w8, [x0, #0x24]
    cmp w8, #4
    b.ne \fallback
    stp x19, x30, [sp, #-80]!
    ldp x8, x9, [x0]
    stp x8, x9, [sp, #16]
    ldp x8, x9, [x0, #16]
    stp x8, x9, [sp, #32]
    ldp x8, x9, [x0, #32]
    stp x8, x9, [sp, #48]
    ldr x8, [x0, #48]
    str x8, [sp, #64]
    str wzr, [sp, #52]              // private arg+24: full-frame aspect only
    add x0, sp, #16
.endm
.balign 16
.global display_class_hook
display_class_hook:
    display_prepare display_class_stock
    bl original_40F35F3C
    ldp x19, x30, [sp], #80
    ret
display_class_stock:
    b original_40F35F3C
.balign 16
.global display_place_hook
display_place_hook:
    display_prepare display_place_stock
    bl original_40F36000
    ldp x19, x30, [sp], #80
    ret
display_place_stock:
    b original_40F36000

.balign 16
.global display_size_hook
display_size_hook:
    cmp w0, #4
    b.ne display_size_stock
    cmp w1, #2
    b.ne display_size_stock
    ldr x8, =OG_STATE
    ldr w8, [x8, #4]
    cbz w8, display_size_stock
    ldr x8, =OG_FLAG
    ldrb w8, [x8]
    cmp w8, #1
    b.ne display_size_stock         // same rule as display_prepare
    adr x0, local_display_size
    ret
display_size_stock:
    b original_41061CCC
.balign 4
local_display_size:
    .short 1024,680
.ltorg
