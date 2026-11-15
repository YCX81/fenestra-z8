/* Frame-buffer registration height for non-16-aligned coded heights (e.g. 1280x852 coded 856).
   Hook: 0x402f3c54 `and w8, w8, #0xfffffff0` (adapter: height = (plan height + 15) & ~15) -> bl cave.
   At that point w0 = decoder instance id (from 0x402f2b94). The stock check 0x40c347fc requires this height
   to equal the height the decoder reported (instance table 0xa02d30164 + id*32, +8). If the reported height
   is smaller by at most 8 (8-aligned coding), use it. Stock sizes are unaffected (reported == aligned). */
    .text
    .globl _start
_start:
    and   w8, w8, #0xfffffff0
    and   x12, x0, #0xff
    movz  x13, #0x0164
    movk  x13, #0x2d3, lsl #16
    movk  x13, #0xa, lsl #32
    add   x13, x13, x12, lsl #5
    ldr   w12, [x13, #8]
    cbz   w12, 1f
    cmp   w12, w8
    b.hs  1f
    sub   w13, w8, w12
    cmp   w13, #8
    b.hi  1f
    mov   w8, w12
1:  ret
