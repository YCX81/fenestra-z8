/* SPDX-License-Identifier: MIT
 * Nikon Z8 C3.11 (z8_xm6_elf0.elf) CEVA-XM6 Core 0 Coexistence Linker Script
 * Links Slot 39/40 (FilmGrain) + Slot 41 (Ricoh Optical HDF) together in-place!
 *
 * Verified against extracted/z8_xm6_elf0.elf (692,516 B = 0xa9124):
 *   - .text.z8fx   : DRAM 0x418c4580 .. 0x418eb000 (<= 158,336 B, strictly <= 158,720 B = 155 KiB budget
 *                    and <= 159,904 B exact non-loaded ELF0 tail). 32-byte aligned, R-X.
 *                    EXPEED 7 hardware fetches 16-byte reversed chunks from SHF_EXECINSTR sections.
 *   - .rodata.z8fx : IDM  0x0003bc40 .. 0x0003d000 (<= 5,056 B, normal little-endian; <= 2 KiB ideal target).
 *                    Zero-wait-state LUT storage; MUST NOT collide with __STACK_SECT @ 0x0003d000.
 *   - .data / .bss : STRICTLY 0 bytes (zero writable global/static state).
 */

ENTRY(Z8_FilmGrain_Slot39_40)

/* Z8 C3.11 ELF0 verified built-in symbols in PMSS (.text.fw.internal, zero-wait IRAM) */
PROVIDE(_ZN8NikonDMA18CreateDescriptor1DEPNS_10DescriptorEjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE = 0x000023c0);
PROVIDE(_ZN8NikonDMA18CreateDescriptor2DEPNS_10DescriptorEjjjjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE = 0x00002480);
PROVIDE(_ZN8NikonDMA21DispatchRequestNoSyncEPvPKvPKNS_10DescriptorE = 0x00002560);
PROVIDE(_ZN8NikonDMA15DispatchRequestEPvPKvPKNS_10DescriptorE = 0x00002960);
PROVIDE(_ZN8NikonDMA11WaitRequestEjj = 0x00002ac0);
PROVIDE(memcpy = 0x00005500);
PROVIDE(memset = 0x00006300);

MEMORY
{
    DRAM_FX    (rx) : ORIGIN = 0x418c4580, LENGTH = 158336  /* 0x418c4580 .. 0x418eb000 (<= 158,720 B 155 KiB cap) */
    IDM_RODATA (r)  : ORIGIN = 0x0003bc40, LENGTH = 5056    /* 0x0003bc40 .. 0x0003d000 */
}

SECTIONS
{
    .text.z8fx 0x418c4580 : ALIGN(32)
    {
        __z8fx_text_start = .;
        KEEP(*(.text.Z8_FilmGrain_Slot39_40))
        KEEP(*(.text.Z8_RicohHdf_Slot41))
        *(.text .text.*)
        . = ALIGN(32);
        __z8fx_text_end = .;
    } > DRAM_FX =0x00000000

    .rodata.z8fx 0x0003bc40 : ALIGN(32)
    {
        __z8fx_rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(16);
        __z8fx_rodata_end = .;
    } > IDM_RODATA

    .data :
    {
        *(.data .data.*)
    } > IDM_RODATA

    .bss (NOLOAD) :
    {
        *(.bss .bss.* COMMON)
    } > IDM_RODATA

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame*)
        *(.note.GNU-stack)
    }
}

ASSERT(SIZEOF(.text.z8fx)         <= 158336, "FATAL: combined .text.z8fx exceeds 0x418eb000 (158,336 B <= 155 KiB)!")
ASSERT((SIZEOF(.text.z8fx) % 32)  == 0,      "FATAL: combined .text.z8fx size must be a 32-byte multiple!")
ASSERT(SIZEOF(.rodata.z8fx)       <= 5056,   "FATAL: combined .rodata.z8fx collides with __STACK_SECT @ 0x0003d000!")
ASSERT(SIZEOF(.data)              == 0,      "FATAL: writable .data must be 0 bytes!")
ASSERT(SIZEOF(.bss)               == 0,      "FATAL: writable .bss must be 0 bytes!")
