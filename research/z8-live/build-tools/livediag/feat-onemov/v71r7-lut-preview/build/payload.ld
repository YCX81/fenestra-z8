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
SECTIONS {
  .live_shutter 0x4019f020 : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .live_shutter) }
  ASSERT(. <= 0x4019ff40, "Live shutter bank exhausted")

  . = 0x401e2000;
  .text : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .text*) }
  .rodata : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .rodata*) }
  live_primary_end = .;
  ASSERT(live_primary_end <= 0x401fcd00, "live_primary collides with PortraitPB cave 0x401FCD00")

  .live_extra 0x401fd218 : { *(EXCLUDE_FILE(*filmsim_*.o *v71r7_b1_*.o *v71r7_b2_*.o) .live_extra) }
  ASSERT(. <= 0x401fe000, "Live extra cave exhausted")

  .v71r7_lut_menu 0x40981000 : {
    *v71r7_b2_*.o(.text*)
    *(.v71r7_lut_menu .v71r7_lut_menu.*)
    *(.filmsim.pw)
    . = ALIGN(16);
    *v71r7_b2_*.o(.rodata*)
  }
  ASSERT(. <= 0x40990000, "Bank 2 v71r7_lut_menu 0x40981000..0x40990000 exhausted")

/* V70 Silky30fps All-in-One .filmsim RX Bank (0x409B0000..0x409D8000, 160 KiB)
 *
 * Keeps Live src-fix37 primary code at 0x401E2000..0x401FCD00 (leaving
 * 0x401FCD00..0x401FCE8C untouched for PortraitPB and 0x408C6000..0x40951000
 * untouched for FontCJK CJK_EXT_1), and places:
 *   - 18 FilmSim EL1 objects (filmsim_01..18)
 *   - 1 V70 NEON Direct NV12/I420 Scaler/Copy/Preview kernel (filmsim_19-live_scale_neon.c.o)
 *   - V70 scalar cold/opt functions tagged section(".filmsim") / section(".filmsim.*")
 *     from src-fix37 (live_w5enc.c, live_stream.c, native_capture.c)
 * into the formally audited zero RX bank 0x409B0000..0x409D8000.
 */

  .filmsim 0x409b0000 : {
    *filmsim_0*.o(.text*)
    *filmsim_1*.o(.text*)
    *(.filmsim .filmsim.*)
    . = ALIGN(16);
    *filmsim_0*.o(.rodata*)
    *filmsim_1*.o(.rodata*)
  }
  ASSERT(. <= 0x409d8000, "FilmSim RX bank 0x409B0000..0x409D8000 exhausted")
  ASSERT(SIZEOF(.data) == 0, "writable static data forbidden")
  ASSERT(SIZEOF(.bss) == 0, "writable static state forbidden")


  .v71r7_lut_core 0x409db000 : {
    *v71r7_b1_*.o(.text*)
    *filmsim_20-z8_xm6_bridge.c.o(.text*)
    *(.v71r7_lut_core .v71r7_lut_core.*)
    . = ALIGN(16);
    *v71r7_b1_*.o(.rodata*)
    *filmsim_20-z8_xm6_bridge.c.o(.rodata*)
  }
  ASSERT(. <= 0x409ec000, "Bank 1 v71r7_lut_core 0x409DB000..0x409EC000 exhausted")

  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
