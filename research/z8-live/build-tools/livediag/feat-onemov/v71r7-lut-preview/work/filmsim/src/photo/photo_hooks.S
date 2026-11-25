/* SPDX-License-Identifier: MIT
 * Thin Z8 C3.11 / V66 photo hook adapters preserving original ABI and registers.
 */
.text
.align 2
.global z8_np_original_resize
.type z8_np_original_resize,%function
z8_np_original_resize:
    stp x28,x27,[sp,#-0x60]!
    ldr x16,=0x40eaafc4
    br x16
.global z8_np_original_thumbnail
.type z8_np_original_thumbnail,%function
z8_np_original_thumbnail:
    stp x28,x26,[sp,#-0x50]!
    ldr x16,=0x40eab76c
    br x16
.global z8_np_original_cleanup
.type z8_np_original_cleanup,%function
z8_np_original_cleanup:
    str x22,[sp,#-0x30]!
    ldr x16,=0x40ea44d4
    br x16

.global z8_np_wait64_stub
.type z8_np_wait64_stub,%function
z8_np_wait64_stub:
    mov x4,x19
    b z8_np_hook_wait64
.global z8_np_wait54_stub
.type z8_np_wait54_stub,%function
z8_np_wait54_stub:
    mov x4,x19
    b z8_np_hook_wait54
.global z8_np_thumbnail_resize_stub
.type z8_np_thumbnail_resize_stub,%function
z8_np_thumbnail_resize_stub:
    mov x1,x21
    b z8_np_hook_thumbnail_resize
.global z8_np_engine19_stub
.type z8_np_engine19_stub,%function
z8_np_engine19_stub:
    mov x2,x19
    mov w3,#0
    b z8_np_hook_engine_ex
.global z8_np_engine19_job2_stub
.type z8_np_engine19_job2_stub,%function
z8_np_engine19_job2_stub:
    mov x2,x19
    mov w3,#1
    b z8_np_hook_engine_ex
.global z8_np_engine21_stub
.type z8_np_engine21_stub,%function
z8_np_engine21_stub:
    mov x2,x21
    mov w3,#0
    b z8_np_hook_engine_ex
.section .note.GNU-stack,"",%progbits
