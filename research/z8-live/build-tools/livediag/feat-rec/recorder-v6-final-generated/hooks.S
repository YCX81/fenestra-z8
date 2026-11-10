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
 mov x19,x0
.global fr_lamp_i
fr_lamp_i:
 mov w1,#0
 add x2,sp,#528
 bl fr_light
 add x9,sp,#528
 ldp x0,x1,[x9]
 ldp x2,x3,[x9,#16]
 ldp x4,x5,[x9,#32]
 ldp x6,x7,[x9,#48]
 ldr x9,[sp,#776]
 msr nzcv,x9
.global fr_notify_call
fr_notify_call:
 bl live_w5_bringup_notify_lamp
.global fr_lamp_r
fr_lamp_r:
 mov x0,x19
 mov w1,#1
 add x2,sp,#528
 bl fr_light
 fr_restore
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

/* Lamp v4: B trampolines; all C runs inside the full 784-byte frame. */
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
.global fr_entry_3
fr_entry_3:
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
 fr_save
 mov w0,#3
 add x1,sp,#528
 bl fr_probe
 fr_restore
92:
.global fr_stock_3
fr_stock_3:
 .inst 0xa9477bf3
 b fr_next_3

.global fr_exit_scoped
fr_exit_scoped:
 stp x22,x30,[sp,#-32]!
 mov x22,x0
 sub x1,sp,#32
 bl fr_nr_begin
 cbz w0,1f
 mov w0,#4
 bl fr_exit_clone
.global fr_exit_return
fr_exit_return:
 str x0,[sp,#16]
 mov x0,x22
 bl fr_nr_end
 ldr x0,[sp,#16]
 ldp x22,x30,[sp],#32
 ret
1:
 mov w0,#4
 ldp x22,x30,[sp],#32
 b fr_native_exit
.global fr_exit_clone
fr_exit_clone:
.global fr_clone_402f3084
fr_clone_402f3084:
.Lclone_402f3084:
 .inst 0xa9be53f5
.global fr_clone_402f3088
fr_clone_402f3088:
.Lclone_402f3088:
 .inst 0xa9017bf3
.global fr_clone_402f308c
fr_clone_402f308c:
.Lclone_402f308c:
 .inst 0x2a0003f3
.global fr_clone_402f3090
fr_clone_402f3090:
.Lclone_402f3090:
 bl fr_native_402f2b94
.global fr_clone_402f3094
fr_clone_402f3094:
.Lclone_402f3094:
 bl fr_native_40c3325c
.global fr_clone_402f3098
fr_clone_402f3098:
.Lclone_402f3098:
 cbz w0,.Lclone_402f30b0
.global fr_clone_402f309c
fr_clone_402f309c:
.Lclone_402f309c:
 .inst 0x320017e0
.global fr_clone_402f30a0
fr_clone_402f30a0:
.Lclone_402f30a0:
 .inst 0x321f03e1
.global fr_clone_402f30a4
fr_clone_402f30a4:
.Lclone_402f30a4:
 .inst 0x2a1f03e2
.global fr_clone_402f30a8
fr_clone_402f30a8:
.Lclone_402f30a8:
 .inst 0x2a1f03e3
.global fr_clone_402f30ac
fr_clone_402f30ac:
.Lclone_402f30ac:
 bl fr_native_40ca1990
.global fr_clone_402f30b0
fr_clone_402f30b0:
.Lclone_402f30b0:
 .inst 0x2a1303e0
.global fr_clone_402f30b4
fr_clone_402f30b4:
.Lclone_402f30b4:
 bl fr_native_402f2bac
.global fr_clone_402f30b8
fr_clone_402f30b8:
.Lclone_402f30b8:
 .inst 0x52802ac0
.global fr_clone_402f30bc
fr_clone_402f30bc:
.Lclone_402f30bc:
 bl fr_native_40c001f4
.global fr_clone_402f30c0
fr_clone_402f30c0:
.Lclone_402f30c0:
 .inst 0xd2909113
.global fr_clone_402f30c4
fr_clone_402f30c4:
.Lclone_402f30c4:
 .inst 0xf2a05d33
.global fr_clone_402f30c8
fr_clone_402f30c8:
.Lclone_402f30c8:
 .inst 0xf2c00153
.global fr_clone_402f30cc
fr_clone_402f30cc:
.Lclone_402f30cc:
 .inst 0xf2e00013
.global fr_clone_402f30d0
fr_clone_402f30d0:
.Lclone_402f30d0:
 .inst 0xf9000260
.global fr_clone_402f30d4
fr_clone_402f30d4:
.Lclone_402f30d4:
 .inst 0x52803bc0
.global fr_clone_402f30d8
fr_clone_402f30d8:
.Lclone_402f30d8:
 bl fr_native_40c001f4
.global fr_clone_402f30dc
fr_clone_402f30dc:
.Lclone_402f30dc:
 .inst 0xd2909214
.global fr_clone_402f30e0
fr_clone_402f30e0:
.Lclone_402f30e0:
 .inst 0xf2a05d34
.global fr_clone_402f30e4
fr_clone_402f30e4:
.Lclone_402f30e4:
 .inst 0xf2c00154
.global fr_clone_402f30e8
fr_clone_402f30e8:
.Lclone_402f30e8:
 .inst 0xf2e00014
.global fr_clone_402f30ec
fr_clone_402f30ec:
.Lclone_402f30ec:
 .inst 0xf9000280
.global fr_clone_402f30f0
fr_clone_402f30f0:
.Lclone_402f30f0:
 .inst 0x52803c80
.global fr_clone_402f30f4
fr_clone_402f30f4:
.Lclone_402f30f4:
 bl fr_native_40c001f4
.global fr_clone_402f30f8
fr_clone_402f30f8:
.Lclone_402f30f8:
 .inst 0xd2909315
.global fr_clone_402f30fc
fr_clone_402f30fc:
.Lclone_402f30fc:
 .inst 0xf9400268
.global fr_clone_402f3100
fr_clone_402f3100:
.Lclone_402f3100:
 .inst 0xf2a05d35
.global fr_clone_402f3104
fr_clone_402f3104:
.Lclone_402f3104:
 .inst 0xf2c00155
.global fr_clone_402f3108
fr_clone_402f3108:
.Lclone_402f3108:
 .inst 0xf2e00015
.global fr_clone_402f310c
fr_clone_402f310c:
.Lclone_402f310c:
 .inst 0xf90002a0
.global fr_clone_402f3110
fr_clone_402f3110:
.Lclone_402f3110:
 .inst 0xaa0803e0
.global fr_clone_402f3114
fr_clone_402f3114:
.Lclone_402f3114:
 bl fr_native_40c2d0f8
.global fr_clone_402f3118
fr_clone_402f3118:
.Lclone_402f3118:
 .inst 0xf9400280
.global fr_clone_402f311c
fr_clone_402f311c:
.Lclone_402f311c:
 bl fr_native_40c2e7fc
.global fr_clone_402f3120
fr_clone_402f3120:
.Lclone_402f3120:
 .inst 0xf94002a0
.global fr_clone_402f3124
fr_clone_402f3124:
.Lclone_402f3124:
 bl fr_native_40c2fb7c
.global fr_clone_402f3128
fr_clone_402f3128:
.Lclone_402f3128:
 .inst 0xf9400260
.global fr_clone_402f312c
fr_clone_402f312c:
.Lclone_402f312c:
 .inst 0x52800861
.global fr_clone_402f3130
fr_clone_402f3130:
.Lclone_402f3130:
 bl fr_native_40c2d884
.global fr_clone_402f3134
fr_clone_402f3134:
.Lclone_402f3134:
 .inst 0xf9400260
.global fr_clone_402f3138
fr_clone_402f3138:
.Lclone_402f3138:
 .inst 0x52800881
.global fr_clone_402f313c
fr_clone_402f313c:
.Lclone_402f313c:
 bl fr_native_40c2d884
.global fr_clone_402f3140
fr_clone_402f3140:
.Lclone_402f3140:
 .inst 0xf9400260
.global fr_clone_402f3144
fr_clone_402f3144:
.Lclone_402f3144:
 .inst 0x528008a1
.global fr_clone_402f3148
fr_clone_402f3148:
.Lclone_402f3148:
 bl fr_native_40c2d884
.global fr_clone_402f314c
fr_clone_402f314c:
.Lclone_402f314c:
 .inst 0xf9400260
.global fr_clone_402f3150
fr_clone_402f3150:
.Lclone_402f3150:
 .inst 0x528008c1
.global fr_clone_402f3154
fr_clone_402f3154:
.Lclone_402f3154:
 bl fr_native_40c2d884
.global fr_clone_402f3158
fr_clone_402f3158:
.Lclone_402f3158:
 .inst 0xf9400260
.global fr_clone_402f315c
fr_clone_402f315c:
.Lclone_402f315c:
 .inst 0x528008e1
.global fr_clone_402f3160
fr_clone_402f3160:
.Lclone_402f3160:
 bl fr_native_40c2d884
.global fr_clone_402f3164
fr_clone_402f3164:
.Lclone_402f3164:
 .inst 0xf9400260
.global fr_clone_402f3168
fr_clone_402f3168:
.Lclone_402f3168:
 .inst 0x52800901
.global fr_clone_402f316c
fr_clone_402f316c:
.Lclone_402f316c:
 bl fr_native_40c2d884
.global fr_clone_402f3170
fr_clone_402f3170:
.Lclone_402f3170:
 .inst 0xf9400260
.global fr_clone_402f3174
fr_clone_402f3174:
.Lclone_402f3174:
 .inst 0x52800921
.global fr_clone_402f3178
fr_clone_402f3178:
.Lclone_402f3178:
 bl fr_native_40c2d884
.global fr_nr_lr_a
fr_nr_lr_a:
.Lnr_lr_a:
.global fr_clone_402f317c
fr_clone_402f317c:
.Lclone_402f317c:
 .inst 0xf9400260
.global fr_clone_402f3180
fr_clone_402f3180:
.Lclone_402f3180:
 .inst 0x52800941
.global fr_clone_402f3184
fr_clone_402f3184:
.Lclone_402f3184:
 fr_save
 mov x0,x22
 add x1,sp,#528
 adr x2,.Lnr_lr_a
 bl fr_nr_allow
 cbz w0,1f
 fr_restore
 b 2f
1:
 fr_restore
 bl fr_native_reset
2:
 adr x30,.Lnr_lr_b
.global fr_nr_lr_b
fr_nr_lr_b:
.Lnr_lr_b:
.global fr_clone_402f3188
fr_clone_402f3188:
.Lclone_402f3188:
 .inst 0xf9400260
.global fr_clone_402f318c
fr_clone_402f318c:
.Lclone_402f318c:
 .inst 0x52800961
.global fr_clone_402f3190
fr_clone_402f3190:
.Lclone_402f3190:
 fr_save
 mov x0,x22
 add x1,sp,#528
 adr x2,.Lnr_lr_b
 bl fr_nr_allow
 cbz w0,1f
 fr_restore
 b 2f
1:
 fr_restore
 bl fr_native_reset
2:
 b fr_native_tail
