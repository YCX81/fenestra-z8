/* Preserve NFP's entry and all ordinary formats. The original helper uses
 * mode0=16:9 and mode1=3:2, irrespective of the supplied source ratio. */
.text
.global live_native_aspect_fix
live_native_aspect_fix:
    cbnz w2,done
    cmp w0,#1024
    b.ne check1280
    cmp w1,#682
    b.eq live32
    b done
check1280:
    cmp w0,#1280
    b.ne done
    cmp w1,#852
    b.ne done
live32:
    mov w2,#1
done:
    ldr x16,=0x409708f0
    br x16
