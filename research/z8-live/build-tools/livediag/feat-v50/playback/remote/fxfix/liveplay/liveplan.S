/* v2: stock movie player plan for HEVC clips of a non-stock size (Live: 1024x682, 1280x852, ...) (C3.11).
   Hook: 0x402f3264 (plan selection, first insn `ldp w9,w11,[x0]`) -> b cave.
   Applies only when: config is HEVC with size class 0, the caller is one of the two playback call sites
   (adapter config, LR 0x402f3e3c; MOVS query via thunk, LR 0x40cf99e4), and the playback UI's current-file
   info (0xa045f08f8: +0xc width u16, +0xe height u16, +0x10 class) has class 9 (= size not in the stock
   table) with 0 < width <= 1920 and 0 < height <= 1088. Then a plan with FHD 30p resources and the file's own
   picture size is written instead of the stock 8K table entry. */
    .text
    .globl _start
_start:
    ldp   w9, w11, [x0]
    cmp   w9, #1
    b.ne  back
    ldr   w10, [x0, #8]
    cbnz  w10, back
    movz  x8, #0x3e3c
    movk  x8, #0x402f, lsl #16
    cmp   x30, x8
    b.eq  1f
    movz  x8, #0x99e4
    movk  x8, #0x40cf, lsl #16
    cmp   x30, x8
    b.ne  back
1:  movz  x8, #0x0904
    movk  x8, #0x45f, lsl #16
    movk  x8, #0xa, lsl #32
    ldr   w10, [x8, #4]
    cmp   w10, #9
    b.ne  back
    ldrh  w10, [x8]
    ldrh  w12, [x8, #2]
    cbz   w10, back
    cbz   w12, back
    cmp   w10, #1920
    b.hi  back
    cmp   w12, #1088
    b.hi  back
    adr   x13, plan
    ldp   x14, x15, [x13]
    stp   x14, x15, [x1]
    ldp   x14, x15, [x13, #16]
    stp   x14, x15, [x1, #16]
    ldr   w14, [x13, #32]
    str   w14, [x1, #32]
    strh  w10, [x1, #6]
    str   w12, [x1, #8]
    mov   x0, x1
    ret
back:
    b     fw_resume
    .balign 8
plan:
    .word 1, 0x00000008, 0, 0x7530, 0x3e9, 0xf, 1, 0x2aea540, 1
