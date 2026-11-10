INCLUDE ui/existing_core.ld
live_requested_set = 0x401ff000;
live_status = 0x401ff004;
live_runtime_boot = 0x401ff008;
live_runtime_event = 0x401ff00c;
SECTIONS {
 . = 0x401e2000;
 .text : { *(.text*) }
 .rodata : { *(.rodata*) }
 .data : { *(.data*) }
 .bss : { *(.bss*) }
 ASSERT(SIZEOF(.data)==0,"writable data")
 ASSERT(SIZEOF(.bss)==0,"writable bss")
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
