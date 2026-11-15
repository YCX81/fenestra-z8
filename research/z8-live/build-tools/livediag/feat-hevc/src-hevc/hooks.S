/* Preserve all integer registers and condition flags at non-call entry hooks.
 * C helpers compile -mgeneral-regs-only, so SIMD/FP state is untouched. */
.text
.macro save_all
 sub sp,sp,#256
 stp x0,x1,[sp,#0]
 stp x2,x3,[sp,#16]
 stp x4,x5,[sp,#32]
 stp x6,x7,[sp,#48]
 stp x8,x9,[sp,#64]
 stp x10,x11,[sp,#80]
 stp x12,x13,[sp,#96]
 stp x14,x15,[sp,#112]
 stp x16,x17,[sp,#128]
 stp x18,x19,[sp,#144]
 stp x20,x21,[sp,#160]
 stp x22,x23,[sp,#176]
 stp x24,x25,[sp,#192]
 stp x26,x27,[sp,#208]
 stp x28,x29,[sp,#224]
 mrs x9,nzcv
 stp x30,x9,[sp,#240]
.endm
.macro restore_all
 ldp x30,x9,[sp,#240]
 msr nzcv,x9
 ldp x28,x29,[sp,#224]
 ldp x26,x27,[sp,#208]
 ldp x24,x25,[sp,#192]
 ldp x22,x23,[sp,#176]
 ldp x20,x21,[sp,#160]
 ldp x18,x19,[sp,#144]
 ldp x16,x17,[sp,#128]
 ldp x14,x15,[sp,#112]
 ldp x12,x13,[sp,#96]
 ldp x10,x11,[sp,#80]
 ldp x8,x9,[sp,#64]
 ldp x6,x7,[sp,#48]
 ldp x4,x5,[sp,#32]
 ldp x2,x3,[sp,#16]
 ldp x0,x1,[sp,#0]
 add sp,sp,#256
.endm
.global live_irq_entry
live_irq_entry:
 save_all
 bl live_irq_notify
 restore_all
 stp x19,x30,[sp,#-16]!
 b live_irq_resume
.global live_press_entry
live_press_entry:
 save_all
 bl live_press_notify
 restore_all
 b live_old_movie_press
.macro epoch_entry name, resume
.global \name
\name:
 save_all
 bl live_epoch_notify
 restore_all
 ldr w8,[x0,#4]
 b \resume
.endm
epoch_entry live_start_entry, live_start_resume
epoch_entry live_stop_entry, live_stop_resume
epoch_entry live_hold_entry, live_hold_resume
.global live_pool_wrap
live_pool_wrap:
 save_all
 bl live_pool_notify
 restore_all
 b live_old_pool
.global live_closed_entry
live_closed_entry:
 strh wzr,[x29,x28]
 save_all
 mov x0,x21
 bl live_closed_notify
 restore_all
 b live_closed_resume
.global live_w5_bringup_entry
live_w5_bringup_entry:
 save_all
 bl live_w5_bringup_notify
 restore_all
 sub sp,sp,#0x80
 b live_w5_bringup_resume
