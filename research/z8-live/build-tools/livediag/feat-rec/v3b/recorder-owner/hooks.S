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
.arch armv8-a+simd
.macro fr_save
 save_all
 sub sp,sp,#528
 stp q0,q1,[sp,#0]
 stp q2,q3,[sp,#32]
 stp q4,q5,[sp,#64]
 stp q6,q7,[sp,#96]
 stp q8,q9,[sp,#128]
 stp q10,q11,[sp,#160]
 stp q12,q13,[sp,#192]
 stp q14,q15,[sp,#224]
 stp q16,q17,[sp,#256]
 stp q18,q19,[sp,#288]
 stp q20,q21,[sp,#320]
 stp q22,q23,[sp,#352]
 stp q24,q25,[sp,#384]
 stp q26,q27,[sp,#416]
 stp q28,q29,[sp,#448]
 stp q30,q31,[sp,#480]
 mrs x9,fpcr
 mrs x10,fpsr
 str x9,[sp,#512]
 str x10,[sp,#520]
.endm
.macro fr_restore
 ldr x9,[sp,#512]
 ldr x10,[sp,#520]
 msr fpcr,x9
 msr fpsr,x10
 ldp q0,q1,[sp,#0]
 ldp q2,q3,[sp,#32]
 ldp q4,q5,[sp,#64]
 ldp q6,q7,[sp,#96]
 ldp q8,q9,[sp,#128]
 ldp q10,q11,[sp,#160]
 ldp q12,q13,[sp,#192]
 ldp q14,q15,[sp,#224]
 ldp q16,q17,[sp,#256]
 ldp q18,q19,[sp,#288]
 ldp q20,q21,[sp,#320]
 ldp q22,q23,[sp,#352]
 ldp q24,q25,[sp,#384]
 ldp q26,q27,[sp,#416]
 ldp q28,q29,[sp,#448]
 ldp q30,q31,[sp,#480]
 add sp,sp,#528
 restore_all
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
 stp x9,x10,[sp,#-32]!
 stp x11,x12,[sp,#16]
 mrs x11,nzcv
 mov x9,#0x50d8
 movk x9,#0x032b,lsl #16
 movk x9,#0xa,lsl #32
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63743
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 mov x10,#63728
 add x9,x9,x10
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#2535
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#64]
 mov w12,#14669
 movk w12,#18010,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#68]
 cbz w10,80f
 ldrh w10,[x9,#32]
 mov w12,#46
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#2528
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#22598
 movk w12,#18258,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#64
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#15
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21833
 movk w12,#19536,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#26511
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21041
 movk w12,#19521,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#26512
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 mov x10,#0x5a90
 add x9,x9,x10
 ldar w10,[x9]
 cbz w10,80f
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 81f
80:
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 93f
81:
 fr_save
 add x0,sp,#528
 bl fr_before
 add x9,sp,#528
 ldp x0,x1,[x9]
 ldp x2,x3,[x9,#16]
 ldp x4,x5,[x9,#32]
 ldp x6,x7,[x9,#48]
 ldr x9,[sp,#776]
 msr nzcv,x9
 bl live_w5_bringup_notify
 mov w0,#1
 add x1,sp,#528
 bl fr_enabled
 cbz w0,94f
 mov w0,#1
 add x1,sp,#528
 bl fr_probe
94: fr_restore
 b 92f
93:
.global fr_bringup_unarmed
fr_bringup_unarmed:
 save_all
 bl live_w5_bringup_notify
 restore_all
92:
.global fr_stock_1
fr_stock_1:
 sub sp,sp,#0x80
 b live_w5_bringup_resume

/* recorder: B entry, never BL at patch sites. */
.global fr_entry_2
fr_entry_2:
 stp x9,x10,[sp,#-32]!
 stp x11,x12,[sp,#16]
 mrs x11,nzcv
 mov x9,#0x50d8
 movk x9,#0x032b,lsl #16
 movk x9,#0xa,lsl #32
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63743
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 mov x10,#63728
 add x9,x9,x10
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#2535
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#64]
 mov w12,#14669
 movk w12,#18010,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#68]
 cbz w10,80f
 ldrh w10,[x9,#32]
 mov w12,#46
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#2528
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#22598
 movk w12,#18258,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#64
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#15
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21833
 movk w12,#19536,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#26511
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21041
 movk w12,#19521,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#26512
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 mov x10,#0x5a90
 add x9,x9,x10
 ldar w10,[x9]
 cbz w10,80f
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 81f
80:
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 92f
81:
 save_all
 mov w0,#2
 mov x1,sp
 bl fr_enabled
 cbz w0,90f
 restore_all
 b 91f
90: restore_all
 b 92f
91:
 fr_save
 mov w0,#2
 add x1,sp,#528
 bl fr_probe
 fr_restore
92:
.global fr_stock_2
fr_stock_2:
 cbnz w0,1f
 b fr_target_2
1: b fr_next_2
.global fr_entry_4
fr_entry_4:
 stp x9,x10,[sp,#-32]!
 stp x11,x12,[sp,#16]
 mrs x11,nzcv
 mov x9,#0x50d8
 movk x9,#0x032b,lsl #16
 movk x9,#0xa,lsl #32
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63743
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 mov x10,#63728
 add x9,x9,x10
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#2535
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#64]
 mov w12,#14669
 movk w12,#18010,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#68]
 cbz w10,80f
 ldrh w10,[x9,#32]
 mov w12,#46
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#2528
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#22598
 movk w12,#18258,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#64
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#15
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21833
 movk w12,#19536,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#26511
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21041
 movk w12,#19521,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#26512
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 mov x10,#0x5a90
 add x9,x9,x10
 ldar w10,[x9]
 cbz w10,80f
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 81f
80:
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 92f
81:
 save_all
 mov w0,#4
 mov x1,sp
 bl fr_enabled
 cbz w0,90f
 restore_all
 b 91f
90: restore_all
 b 92f
91:
 fr_save
 mov w0,#4
 add x1,sp,#528
 bl fr_probe
 fr_restore
92:
.global fr_stock_4
fr_stock_4:
 .inst 0xa9bf7bf3
 b fr_next_4
.global fr_entry_5
fr_entry_5:
 stp x9,x10,[sp,#-32]!
 stp x11,x12,[sp,#16]
 mrs x11,nzcv
 mov x9,#0x50d8
 movk x9,#0x032b,lsl #16
 movk x9,#0xa,lsl #32
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63743
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 mov x10,#63728
 add x9,x9,x10
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#2535
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#64]
 mov w12,#14669
 movk w12,#18010,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#68]
 cbz w10,80f
 ldrh w10,[x9,#32]
 mov w12,#46
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#2528
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#22598
 movk w12,#18258,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#64
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#15
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21833
 movk w12,#19536,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#26511
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21041
 movk w12,#19521,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#26512
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 mov x10,#0x5a90
 add x9,x9,x10
 ldar w10,[x9]
 cbz w10,80f
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 81f
80:
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 92f
81:
 save_all
 mov w0,#5
 mov x1,sp
 bl fr_enabled
 cbz w0,90f
 restore_all
 b 91f
90: restore_all
 b 92f
91:
 fr_save
 mov w0,#5
 add x1,sp,#528
 bl fr_probe
 fr_restore
92:
.global fr_stock_5
fr_stock_5:
 .inst 0x320003e1
 b fr_next_5
.global fr_entry_13
fr_entry_13:
 stp x9,x10,[sp,#-32]!
 stp x11,x12,[sp,#16]
 mrs x11,nzcv
 mov x9,#0x50d8
 movk x9,#0x032b,lsl #16
 movk x9,#0xa,lsl #32
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63743
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 mov x10,#63728
 add x9,x9,x10
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#2535
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#64]
 mov w12,#14669
 movk w12,#18010,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#68]
 cbz w10,80f
 ldrh w10,[x9,#32]
 mov w12,#46
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#2528
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#22598
 movk w12,#18258,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#64
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#15
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21833
 movk w12,#19536,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#26511
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21041
 movk w12,#19521,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#26512
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 mov x10,#0x5a90
 add x9,x9,x10
 ldar w10,[x9]
 cbz w10,80f
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 81f
80:
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 92f
81:
 save_all
 mov w0,#13
 mov x1,sp
 bl fr_enabled
 cbz w0,90f
 restore_all
 b 91f
90: restore_all
 b 92f
91:
 fr_save
 mov w0,#13
 add x1,sp,#528
 bl fr_probe
 fr_restore
92:
.global fr_stock_13
fr_stock_13:
 tbnz w19,#1,1f
 b fr_target_13
1: b fr_next_13
.global fr_entry_14
fr_entry_14:
 stp x9,x10,[sp,#-32]!
 stp x11,x12,[sp,#16]
 mrs x11,nzcv
 mov x9,#0x50d8
 movk x9,#0x032b,lsl #16
 movk x9,#0xa,lsl #32
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63743
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 mov x10,#63728
 add x9,x9,x10
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#2535
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#64]
 mov w12,#14669
 movk w12,#18010,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#68]
 cbz w10,80f
 ldrh w10,[x9,#32]
 mov w12,#46
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#2528
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#63
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#22598
 movk w12,#18258,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#64
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#15
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21833
 movk w12,#19536,lsl #16
 cmp w10,w12
 b.ne 80f
 add x9,x9,#8
 ldar x9,[x9]
 lsr x10,x9,#30
 cmp x10,#40
 b.ne 80f
 tst x9,#7
 b.ne 80f
 mov x10,#26511
 add x10,x9,x10
 lsr x10,x10,#30
 cmp x10,#40
 b.ne 80f
 ldr w10,[x9,#0]
 mov w12,#21041
 movk w12,#19521,lsl #16
 cmp w10,w12
 b.ne 80f
 ldr w10,[x9,#4]
 mov w12,#26512
 movk w12,#0,lsl #16
 cmp w10,w12
 b.ne 80f
 mov x10,#0x5a90
 add x9,x9,x10
 ldar w10,[x9]
 cbz w10,80f
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 81f
80:
 msr nzcv,x11
 ldp x11,x12,[sp,#16]
 ldp x9,x10,[sp],#32
 b 92f
81:
 save_all
 mov w0,#14
 mov x1,sp
 bl fr_enabled
 cbz w0,90f
 restore_all
 b 91f
90: restore_all
 b 92f
91:
 fr_save
 mov w0,#14
 add x1,sp,#528
 bl fr_probe
 fr_restore
92:
.global fr_stock_14
fr_stock_14:
 .inst 0xd10683ff
 b fr_next_14
