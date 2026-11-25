/* D5 = build-8 hooks (D3 core, D4 menu/preview/icon repairs, i-menu) plus the magnify
 * navigation inset.
 *
 * Body evidence (D4, 2026-10-26 IMG_0119, registered against the same-scene FX still):
 * the magnified window itself already reached row 5454 of 5504, beyond the 16:9 band
 * (rows 432..5072).  The inset that shows the window position was drawn for a 16:9
 * frame: native 4041333C takes the frame size from property 2124h (area 4, 8256x4640),
 * picks the 16:9 inset layout from the GUI aspect class DAT_a032cfc70 and, for windows in
 * the 432-row bands, saturates the yellow box at the inset border.
 *
 * Native check on the QEMU GUI state (OG standby, zoom level 1): unchanged native code
 * puts the inset at 142x82 and the box of any window with top < 432 or bottom > 5072 at
 * the border.  These hooks give the inset the 3:2 layout and the full 8256x5504 frame
 * only while the selected open-gate video mode is current, with the same condition as
 * the D3 zoom reads (OG_STATE+4 or the selected-video cache), so ordinary video and photo
 * use are untouched.  The first site reuses zoom_area_hook (2124h==4 -> 0).
 */
#include "ui_hooks.S"

.macro og_standby_project reg
    stp x16, x17, [sp, #-16]!
    ldr x16, =OG_STATE
    ldr w16, [x16, #4]
    cbnz w16, 1f
    require_selected_video 2f
1:  mov \reg, #0
2:  ldp x16, x17, [sp], #16
.endm

/* ldrb w9,[x8,#0x20]: GUI aspect class (1 = 16:9 inset layout).  Two path copies. */
.balign 16
.global nav_class_a_hook
nav_class_a_hook:
    ldrb w9, [x8, #0x20]
    og_standby_project w9
    b original_40413508
.balign 16
.global nav_class_b_hook
nav_class_b_hook:
    ldrb w9, [x8, #0x20]
    og_standby_project w9
    b original_40413578

/* ldrb w21,[x8,#0x168]: aspect variant (2 = 5:4, 3 = 1:1, 4 = 16:9); 0 selects the 3:2 rectangle. */
.balign 16
.global nav_aspect_a_hook
nav_aspect_a_hook:
    ldrb w21, [x8, #0x168]
    og_standby_project w21
    b original_40413534
.balign 16
.global nav_aspect_b_hook
nav_aspect_b_hook:
    ldrb w21, [x8, #0x168]
    og_standby_project w21
    b original_404135c4
.ltorg
