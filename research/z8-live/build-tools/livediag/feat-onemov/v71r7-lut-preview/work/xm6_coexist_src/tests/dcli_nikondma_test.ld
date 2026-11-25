ENTRY(__cxd_inttbl_start)

__malloc_size        = 1M;
__stack_size         = 12k;
__arg_sect_size      = 400;

MEMORY
{
    INTERNAL_CODE       (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00100000
    IDM_RODATA_Z8FX     (r)   : ORIGIN = 0x0003bc40, LENGTH = 0x00000800
    IDM_STACK           (rw)  : ORIGIN = 0x0003d000, LENGTH = 0x00003000
    EXTERNAL_SRAM       (rwx) : ORIGIN = 0x10000000, LENGTH = 0x10000000
    DRAM_Z8FX           (rx)  : ORIGIN = 0x418c4580, LENGTH = 0x00026a80
    DRAM_EXTWS          (rw)  : ORIGIN = 0x41930000, LENGTH = 0x00080000
    DRAM_TEST           (rw)  : ORIGIN = 0x42000000, LENGTH = 0x01000000
}

SECTIONS {
    .inttbl_sec 0x00000000 : {
        KEEP(*(.inttbl))
        *(.text.__call_saved*)
        *(.text.__cxd_call_saved*)
    } >INTERNAL_CODE

    /* Place the EXACT shipped coexist_nikondma objects at their real Z8 Core 0 VMAs */
    .text.z8fx 0x418c4580 : ALIGN(32) {
        KEEP(*(.text.Z8_FilmGrain_Slot39_40))
        KEEP(*(.text.Z8_RicohHdf_Slot41))
        *zf_grain_nikondma.o(.text .text.*)
        *ricoh_hdf_nikondma.o(.text .text.*)
        . = ALIGN(32);
    } >DRAM_Z8FX

    .rodata.z8fx 0x0003bc40 : ALIGN(32) {
        *zf_grain_nikondma.o(.rodata .rodata.*)
        *ricoh_hdf_nikondma.o(.rodata .rodata.*)
        . = ALIGN(16);
    } >IDM_RODATA_Z8FX

    .text : ALIGN(0x20) {
        PROVIDE(__text_start = ABSOLUTE(.));
        *(.text .text.*)
        PROVIDE(__text_end = ABSOLUTE(.));
    } >INTERNAL_CODE

    .data.internal : ALIGN(0x20) {
        PROVIDE(__data_internal_start = ABSOLUTE(.));
        . += 0x8;
        PROVIDE(__data_start = ABSOLUTE(.));
        *(.data .data.*)
        PROVIDE(__data_end = ABSOLUTE(.));
        PROVIDE(__data_size = +__data_end - __data_start);
        PROVIDE(__sdata_start = ABSOLUTE(.));
        *(.sdata .sdata.*)
        PROVIDE(__sdata_end = ABSOLUTE(.));
        PROVIDE(__sdata_size = +__sdata_end - __sdata_start);
        PROVIDE(__data_internal_end = ABSOLUTE(.));
        PROVIDE(__data_internal_size = ABSOLUTE(__data_internal_end - __data_internal_start));
    } >EXTERNAL_SRAM

    .data.internal.clone (NOLOAD) : ALIGN(0x20) {
        PROVIDE(__data_internal_clone_start = ABSOLUTE(.));
        . = ABSOLUTE(. + __data_internal_size);
    } >EXTERNAL_SRAM
    data_internal_loadable_addr = __data_internal_clone_start;

    .data.internal.ro : ALIGN(0x20) {
        PROVIDE(__data_internal_ro_start = ABSOLUTE(.));
        PROVIDE(__rodata_start = ABSOLUTE(.));
        *(.rodata .rodata.*)
        PROVIDE(__rodata_end = ABSOLUTE(.));
        PROVIDE(__rodata_size = +__rodata_end - __rodata_start);
        PROVIDE(__data_internal_ro_end = ABSOLUTE(.));
        PROVIDE(__data_internal_ro_size = __data_internal_ro_end - __data_internal_ro_start);
    } >EXTERNAL_SRAM

    .bss (NOLOAD) : ALIGN(0x20) {
        PROVIDE(__bss_start = ABSOLUTE(.));
        *(.bss .bss.*)
        PROVIDE(__common_start = ABSOLUTE(.));
        *(COMMON)
        PROVIDE(__common_end = ABSOLUTE(.));
        PROVIDE(__common_size = +__common_end - __common_start);
        PROVIDE(__bss_end = ABSOLUTE(.));
        PROVIDE(__bss_size = +__bss_end - __bss_start);
    } >EXTERNAL_SRAM

    .bss1 (NOLOAD) : ALIGN(0x20) {
        PROVIDE(__bss1_start = ABSOLUTE(.));
        *(.bss1 .bss1.*)
        PROVIDE(__large_common_start = ABSOLUTE(.));
        *(LARGE_COMMON)
        PROVIDE(__large_common_end = ABSOLUTE(.));
        PROVIDE(__large_common_size = +__large_common_end - __large_common_start);
        PROVIDE(__bss1_end = ABSOLUTE(.));
        PROVIDE(__bss1_size = +__bss1_end - __bss1_start);
    } >EXTERNAL_SRAM

    .data.external : ALIGN(0x20) {
        PROVIDE(__data_external_start = ABSOLUTE(.));
        PROVIDE(__data1_start = ABSOLUTE(.));
        *(.data1 .data1.*)
        PROVIDE(__data1_end = ABSOLUTE(.));
        PROVIDE(__sdata1_start = ABSOLUTE(.));
        *(.sdata1 .sdata1.*)
        PROVIDE(__sdata1_end = ABSOLUTE(.));
        PROVIDE(__sdata1_size = +__sdata1_end - __sdata1_start);
        PROVIDE(__data_external_end = ABSOLUTE(.));
        PROVIDE(__data_external_size = ABSOLUTE(__data_external_end - __data_external_start));
    } >EXTERNAL_SRAM

    .data.external.clone (NOLOAD) : ALIGN(0x20) {
        PROVIDE(__data_external_clone_start = ABSOLUTE(.));
        . = ABSOLUTE(. + __data_external_size);
    } >EXTERNAL_SRAM
    data_external_loadable_addr = __data_external_clone_start;

    .data.external.ro : ALIGN(0x20) {
        PROVIDE(__data_external_ro_start = ABSOLUTE(.));
        PROVIDE(__rodata1_start = ABSOLUTE(.));
        *(.rodata1 .rodata1.*)
        PROVIDE(__rodata1_end = ABSOLUTE(.));
        PROVIDE(__rodata1_size = +__rodata1_end - __rodata1_start);
        . = ALIGN(4);
        PROVIDE(__init_array_start = ABSOLUTE(.));
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)));
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array*) SORT_BY_INIT_PRIORITY(.ctors*)));
        PROVIDE(__init_array_end = ABSOLUTE(.));
        PROVIDE(__fini_array_start = ABSOLUTE(.));
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)));
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array*) SORT_BY_INIT_PRIORITY(.dtors*)));
        PROVIDE(__fini_array_end = ABSOLUTE(.));
        PROVIDE(__arg_sect_start = .);
        . = . + (__arg_sect_size ? __arg_sect_size + 4 : 0);
        PROVIDE(__arg_sect_end = .);
        PROVIDE(__data_external_ro_end = ABSOLUTE(.));
        PROVIDE(__data_external_ro_size = __data_external_ro_end - __data_external_ro_start);
    } >EXTERNAL_SRAM

    .ext_ws_sentinel 0x41930000 (NOLOAD) : { . += 0x80000; } >DRAM_EXTWS

    .ext_test_bufs (NOLOAD) : ALIGN(64) {
        *(.ext_test_bufs)
    } >DRAM_TEST

    __STACK_SECT (NOLOAD) : ALIGN(0x10) {
        KEEP(*(__stack_start));
        __stack_start = ABSOLUTE(.);
        . = . + __stack_size;
        KEEP(*(__stack_end));
        __stack_end = ABSOLUTE(.);
    } >IDM_STACK

    __MALLOC_SECT (NOLOAD) : ALIGN(0x10) {
        KEEP(*(__malloc_start));
        __malloc_start = ABSOLUTE(.);
        . = . + __malloc_size;
        KEEP(*(__malloc_end));
        __malloc_end = ABSOLUTE(.);
    } >EXTERNAL_SRAM

    /DISCARD/ : {
        *(.comment)
        *(.note.GNU-stack)
        *(.ceva_reloc)
    }
}
