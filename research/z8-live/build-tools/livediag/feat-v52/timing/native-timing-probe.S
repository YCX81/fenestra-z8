// Standalone QEMU virt driver. Guest code never enters the Z8 OS.
.text
.align 4
_start:
    ldr x0, =0x4f000000
    mov sp, x0
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb
    // 39-bit virtual address space with two 1 GiB normal-memory blocks.
    // The isolated firmware workspace at 0xa00000000 gets separate guest RAM.
    ldr x0, =0x4f001000
    ldr x1, =0x40000701
    str x1, [x0, #8]
    ldr x1, =0x80000701
    str x1, [x0, #320]
    msr ttbr0_el1, x0
    mov x0, #0xff
    msr mair_el1, x0
    ldr x0, =0x200803519
    msr tcr_el1, x0
    dsb sy
    isb
    mrs x0, sctlr_el1
    orr x0, x0, #1
    msr sctlr_el1, x0
    isb

    // Initialize the guest's table from the image's 97-row initialized data.
    ldr x0, =0x44dea658
    ldr x1, =0xa0297f658
    mov x2, #(97 * 8)
1:  ldr x3, [x0], #8
    str x3, [x1], #8
    subs x2, x2, #1
    b.ne 1b
    ldr x16, =0x406ce364
    blr x16

    // In this guest only, redirect the common cleanup to the stock epilogue.
    // The file service and its lock were not entered in this isolated replay.
    ldr x0, =0x406bf1f8
    ldr w1, =0x14000008
    str w1, [x0]
    dc cvau, x0
    dsb sy
    ic ivau, x0
    dsb sy
    isb
    ldr x24, =0x48000000  // packed [u32 length][udta][zero pad to 4] ... [0]
next_case:
    ldr w0, [x24], #4
    cbz w0, init_file_callbacks
    add w0, w0, #3
    bic w0, w0, #3
    mov x1, x24
    add x24, x24, x0
    bl parse
    bl print_word
    ldr x0, =0xa045606a0
    ldr w0, [x0]
    bl print_word
    mov x25, #0
next_image:
    mov x0, x25
    ldr x1, =0x4e000000
    add x2, x1, #2
    add x3, x1, #4
    ldr x16, =0x406c0dd0
    blr x16
    bl print_word
    ldr x1, =0x4e000000
    ldr w0, [x1]
    bl print_word
    ldr x1, =0x4e000000
    ldr w0, [x1, #4]
    bl print_word
    add x25, x25, #1
    cmp x25, #4
    b.ne next_image
    b next_case
init_file_callbacks:
    // Only the file adapter is substituted. Its two guest function pointers
    // are the same indirections used by 0x40ce6a48 and 0x40ce6a6c.
    ldr x0, =0xa031937a0
    adr x1, seek_absolute
    adr x2, read_exact
    stp x1, x2, [x0]
next_movie:
    ldr w0, [x24], #4
    cbz w0, finish
    ldr x8, =0x4e001000
    stp x24, x0, [x8]
    str xzr, [x8, #16]    // current file offset
    add x0, x0, #3
    bic x0, x0, #3
    add x24, x24, x0
    // Clear the caller's private parser workspace for each independent open.
    ldr x0, =0x4c000000
    mov w1, #0
    mov w2, #0x500000
    ldr x16, =0x4127ab40
    blr x16
    mov w0, #1            // synthetic open-file handle
    ldr x1, =0x4c000000
    ldr x2, =0x4e002000   // parsed object output pointer
    mov w3, #0x500000     // workspace capacity
    mov w4, #0            // MOV mode as called via 0x40344b20
    ldr x16, =0x40ce8a08
    blr x16
    mov w25, w0
    bl print_word
    cbnz w25, next_movie
    ldr x0, =0x4e002000
    ldr x0, [x0]
    ldr x1, =0x4e003000
    add x2, x1, #4
    ldr x16, =0x40ce7dc0
    blr x16
    bl print_word
    ldr x8, =0x4e003000
    ldr w0, [x8]
    bl print_word
    ldr x8, =0x4e003000
    ldr w0, [x8, #4]
    bl print_word
    // Populate the two fields copied by stock callback 40cfc090 from its
    // successful-open payload +14/+18. No parser bytes are modified.
    ldr x8, =0x4e003000
    ldp w9, w10, [x8]
    ldr x8, =0xa0319f284
    stp w9, w10, [x8]
    ldr x8, =0xa0319b390
    mov w9, #1
    str w9, [x8]
    // Native display-rate selector from the exact parser ratio.
    ldr x8, =0x4e003000
    ldp w1, w0, [x8]
    ldr x16, =0x40d02bec
    blr x16
    bl print_word
    mov w21, #0
frame_loop:
    adr x8, frame_indices
    ldr w0, [x8, x21, lsl #2]
    ldr x1, =0x4e003010
    ldr x16, =0x40cf4114
    blr x16
    ldr x8, =0x4e003010
    ldr w0, [x8]
    bl print_word
    add w21, w21, #1
    cmp w21, #6
    b.lo frame_loop
    // Standard CFR control, through identical native getter/time functions.
    ldr x8, =0xa0319f284
    mov w9, #1001
    mov w10, #30000
    stp w9, w10, [x8]
    mov w0, #30000
    mov w1, #1001
    ldr x16, =0x40d02bec
    blr x16
    bl print_word
    mov w0, #77
    ldr x1, =0x4e003010
    ldr x16, =0x40cf4114
    blr x16
    ldr x8, =0x4e003010
    ldr w0, [x8]
    bl print_word
    b next_movie
frame_indices: .word 0,1,15,30,60,95
    ldr x0, =0x4e002000
    ldr x0, [x0]
#ifdef PROBE_HEVC_HEADERS
    // Use the parsed movie handle to extract the three parameter sets.
    ldr x21, =0x4e002040
    mov x1, x21
    add x2, x21, #8
    add x3, x21, #16
    add x4, x21, #24
    add x5, x21, #32
    add x6, x21, #40
    ldr x16, =0x40ce8470
    blr x16
    mov w25, w0
    bl print_word
    cbnz w25, next_movie
    mov w22, #3
hevc_param:
    ldr x8, [x21]
    ldrb w0, [x8]
    ubfx w0, w0, #1, #6
    bl print_word
    ldr w0, [x21, #8]
    bl print_word
    add x21, x21, #16
    subs w22, w22, #1
    b.ne hevc_param
    b next_movie
#endif
    mov w1, #0x44          // worker's udta id
    ldr x2, =0x4e002008
    str wzr, [x2]
    ldr x16, =0x40ce6e8c
    blr x16
    mov w25, w0
    bl print_word
    cbnz w25, next_movie
    ldr x8, =0x4e002008
    ldr w0, [x8]
    bl print_word
    ldr x0, =0x4e002000
    ldr x0, [x0]
    mov w1, #0x44
    ldr x2, =0x4d000000
    ldr x16, =0x40ce6d6c
    blr x16
    mov w25, w0
    bl print_word
    cbnz w25, next_movie
    ldr x1, =0x4d000000
    bl parse
    bl print_word
    ldr x8, =0xa045606a0
    ldr w0, [x8]
    bl print_word
    b next_movie
seek_absolute:
    cbnz w2, io_failed
    ldr x8, =0x4e001000
    ldr x9, [x8, #8]
    cmp x1, x9
    b.hi io_failed
    str x1, [x8, #16]
    mov w0, #1
    ret
read_exact:
    ldr x8, =0x4e001000
    ldp x9, x10, [x8]      // base and EOF
    ldr x11, [x8, #16]
    sub x10, x10, x11
    cmp x2, x10
    b.hi io_failed
    add x9, x9, x11
    add x11, x11, x2
    str x11, [x8, #16]
    cbz w2, io_done
4:  ldrb w10, [x9], #1
    strb w10, [x1], #1
    subs w2, w2, #1
    b.ne 4b
io_done:
    mov w0, #1
    ret
io_failed:
    mov w0, #0
    ret
finish:
    ldr x1, =0x4e000000
    ldr x0, =0x20026
    stp x0, xzr, [x1]
    mov x0, #0x20
    hlt #0xf000
hang: b hang

parse:
    // Duplicate only the stack/register inputs live at 0x406bf244, including
    // parser initial state from 0x406bf028..094. Its own epilogue restores them.
    sub sp, sp, #0x80
    stp x28, x27, [sp, #0x20]
    stp x26, x25, [sp, #0x30]
    stp x24, x23, [sp, #0x40]
    stp x22, x21, [sp, #0x50]
    stp x20, x19, [sp, #0x60]
    stp x29, x30, [sp, #0x70]
    mov x22, x1
    ldr x19, =0xa04560628
    mov x8, x19
    mov w9, #15
2:  str xzr, [x8], #8
    subs w9, w9, #1
    b.ne 2b
    ldr x27, =0xa045606a0
    mov w8, #2
    str w8, [x27]
    ldr x28, =0xa045606b0
    str xzr, [x28]
    mov w21, #0x500000
    ldr x16, =0x406bf244
    br x16

print_word:
    // Semihost WRITE0 to console. No guest file open or write.
    ldr x1, =0x4e000100
    add x2, x1, #8
    strh wzr, [x2]
    mov w3, #10
    strb w3, [x2]
    mov w4, #8
3:  and w3, w0, #15
    cmp w3, #10
    add w3, w3, #48
    add w5, w3, #39
    csel w3, w3, w5, lo
    strb w3, [x2, #-1]!
    lsr w0, w0, #4
    subs w4, w4, #1
    b.ne 3b
    mov x0, #4
    hlt #0xf000
    ret
