/* Prototype: replace only the default 8K initial crop for the two Live sizes.
 * 40cf7d08 has w9=stock width,w8=stock height,x0=decoder config,x19=crop state.
 * The native scaler still computes all coefficients and output dimensions.
 * No MMIO access, no clamp at the low-level scaler, no changed resource plan.
 */
.text
.global live_native_crop_fix
live_native_crop_fix:
    stp x10,x11,[sp,#-16]!
    mov w10,#7680
    cmp w9,w10
    b.ne done
    mov w10,#4320
    cmp w8,w10
    b.ne done
    ldr w10,[x0,#8]
    cbnz w10,done
    ldr w10,[x0,#4]
    cmp w10,#1
    b.hi done
    ldr x10,=0xa045f08f8
    ldr w11,[x10,#12]
    mov w10,#1024
    movk w10,#682,lsl #16
    cmp w11,w10
    b.eq live_size
    mov w10,#1280
    movk w10,#852,lsl #16
    cmp w11,w10
    b.ne done
live_size:
    and w9,w11,#0xffff
    lsr w8,w11,#16
done:
    ldp x10,x11,[sp],#16
    strh w9,[x19,#0x14]
    ldr x16,=0x40cf7d0c
    br x16
