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
