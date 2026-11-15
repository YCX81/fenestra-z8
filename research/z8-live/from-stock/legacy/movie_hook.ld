SECTIONS { . = 0x40a02000; .text : { *(.text.entry) *(.text*) *(.rodata*) *(.data*) *(.bss*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }
