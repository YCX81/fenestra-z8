fx_memset = 0x401c689c;
fx_memcpy = 0x401c68f0;
fx_memcmp = 0x401c6958;
fx_native_vg_create_path = 0x401a5724;
fx_scene_validate = 0x401a0a64;
fx_native_menu_build = 0x401b846c;
fx_native_menu_link = 0x401b810c;
fx_memmove = 0x401f0100;
fx_glyph = 0x401f0000;
lp_preview_open = 0x401f63fc;
lp_preview_index = 0x401f65ac;
lp_clip_tail = 0x401f6604;
lp_clip_open = 0x401f668c;
lp_clip_audio = 0x401f68c4;
lp_jpeg_decode = 0x401f69c4;
lp_audio_output_profile = 0x401f7368;
memcpy = 0x401c68f0;
memset = 0x401c689c;
memmove = 0x401f0100;
memcmp = 0x401c6958;
pb_key_resume = 0x41020848;
pb_system_resume = 0x40d60ed0;
pb_delete_resume = 0x4085c428;
pb_protect_resume = 0x4085bcb4;
pb_settings_page_resume = 0x401cce88;
pb_settings_text_resume = 0x401cd3f0;
pb_leave_resume = 0x4061a728;
pb_state_resume = 0x40619de8;
pb_ui_old_poll = 0x401d6948;
pb_ui_old_wait = 0x401d6a4c;
ENTRY(pb_key_entry)
SECTIONS { . = 0x40196000; .text : { *(.text*) } .rodata : { *(.rodata*) }
.data : { *(.data*) } .bss : { *(.bss*) *(COMMON) }
ASSERT(. <= 0x4019f020, "padding overflow")
ASSERT(SIZEOF(.bss) == 0 && SIZEOF(.data) == 0, "no writable RX state")
/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }
