.text
.global fx_z8_system_original
.type fx_z8_system_original,%function
fx_z8_system_original:
#include "fx_z8_system_original.inc"
    ldr x16,1f
    br x16
    .balign 8
1: .quad 0x40d60ed8
.global fx_z8_navigate_original
.type fx_z8_navigate_original,%function
fx_z8_navigate_original:
#include "fx_z8_navigate_original.inc"
    ldr x16,2f
    br x16
    .balign 8
2: .quad 0x403af54c
