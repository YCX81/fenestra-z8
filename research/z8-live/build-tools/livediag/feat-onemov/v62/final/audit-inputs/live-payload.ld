fx_memset = 0x401c689c;
fx_native_menu_build = 0x401b846c;
fx_native_menu_link = 0x401b810c;
fx_native_display_attach_global = 0x401a3c70;
fx_native_display_detach = 0x401a4040;
fx_native_display_is_current = 0x401a296c;
fx_native_display_present = 0x401a3d24;
memcpy = 0x401c6cb4;
memmove = 0x401c6d24;
memset = 0x401c6cd0;
memcmp = 0x401c6958;
live_old_menu_page = 0x401cce84;
live_old_menu_text = 0x401cd3ec;
live_old_system_dispatch = 0x401cd788;
live_old_movie_press = 0x40a020c4;
live_irq_resume = 0x40e9083c;
live_old_writer = 0x409f262c;
live_closed_resume = 0x408b7c20;
live_start_resume = 0x40c26808;
live_stop_resume = 0x40c2681c;
live_hold_resume = 0x40c2682c;
live_old_pool = 0x4104356c;
live_w5_bringup_resume = 0x402f2d5c;
live_stock_shutter_prepare = 0x40f9cccc;
live_stock_shutter_fire = 0x40f9cd54;
ENTRY(live_ui_system_dispatch)
SECTIONS { .live_shutter 0x4019f020 : { *(.live_shutter) }
ASSERT(. <= 0x4019ff40, "Live shutter bank exhausted")
. = 0x401e2000; .text : { *(.text*) } .rodata : { *(.rodata*) }
.data : { *(.data*) } .bss : { *(.bss*) *(COMMON) }
ASSERT(. <= 0x401fd000, "RX padding exhausted")
.live_extra 0x401fd218 : { *(.live_extra) }
ASSERT(. <= 0x401fe000, "Live extra cave exhausted")
ASSERT(SIZEOF(.data) == 0, "writable static data forbidden")
ASSERT(SIZEOF(.bss) == 0, "writable static state forbidden")
/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }
