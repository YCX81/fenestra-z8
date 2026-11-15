/* Native entry hooks preserve integer registers, NZCV, SIMD and original LR.
 * All helpers are compiled mgeneral-regs-only. */
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
.global pb_key_entry
pb_key_entry:
 b pb_key_decode_wrap
.global pb_key_original
pb_key_original:
 sub sp,sp,#0xb0
 b pb_key_resume
.global pb_leave_entry
pb_leave_entry:
 save_all
 bl pb_ui_leave
 restore_all
 sub sp,sp,#0x40
 b pb_leave_resume
.global pb_state_entry
pb_state_entry:
 save_all
 bl pb_ui_state_notify
 restore_all
 sub sp,sp,#0x70
 b pb_state_resume
.global pb_system_entry
pb_system_entry:
 save_all
 bl pb_system_notify
 restore_all
 stp x23,x22,[sp,#0x10]
 b pb_system_resume
.global pb_delete_original
pb_delete_original:
 stp x21,x20,[sp,#-0x20]!
 b pb_delete_resume
.global pb_protect_original
pb_protect_original:
 str x22,[sp,#-0x30]!
 b pb_protect_resume
.global pb_settings_old_menu_page
pb_settings_old_menu_page:
 stp x29,x30,[sp,#-0x30]!
 b pb_settings_page_resume
.global pb_settings_old_menu_text
pb_settings_old_menu_text:
 stp x29,x30,[sp,#-0x40]!
 b pb_settings_text_resume
