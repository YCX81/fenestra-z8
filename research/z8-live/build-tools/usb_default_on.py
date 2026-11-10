#!/usr/bin/env python3
"""USB Debug on by default (Z8 C3.11 Fenestra bridge64 core inside a composed MAIN).

The core's PTP poll (fx_z8_ptp_poll, 0x401dabd8) tail-calls fx_ptp_poll only while
Fenestra is active. One instruction of that path (`mov x0, x19` at 0x401dac34) becomes
`bl CAVE`. The cave enables PTP once per boot: FxPtp has 4 padding bytes at +0x24
(request_bytes at +0x20, uint64 session at +0x28), zeroed by fx_ptp_init_target and
never touched, which it uses as the "auto-enabled" mark. Turning USB Debug off in the
menu afterwards stays off until the next boot.

The cave lives in the last 0x80 bytes of the RX padding after the Live payload
(0x401fff80); payload.ld must keep `. <= 0x401fff80`.
Usage: usb_default_on.py IN.main OUT.main
"""
import struct
import sys

B = 0x40160000
HOOK, CAVE = 0x401dac34, 0x401fff80
PTP_ENABLE = 0x401d90b4
MOV_X0_X19 = 0xaa1303e0


def bl(at, target):
    off = (target - at) // 4
    assert -(1 << 25) <= off < (1 << 25)
    return 0x94000000 | (off & 0x3ffffff)


def cave():
    w = [
        0xa9bf7bfd,  # stp x29, x30, [sp, #-16]!
        0xb9402668,  # ldr w8, [x19, #0x24]      auto-enabled mark
        None,        # cbnz w8, done
        0x52800028,  # mov w8, #1
        0xb9002668,  # str w8, [x19, #0x24]
        0xb9400268,  # ldr w8, [x19]             already enabled?
        None,        # cbnz w8, done
        0xaa1303e0,  # mov x0, x19
        0x52800021,  # mov w1, #1
        None,        # bl fx_ptp_enable
        0xaa1303e0,  # done: mov x0, x19
        0xa8c17bfd,  # ldp x29, x30, [sp], #16
        0xd65f03c0,  # ret
    ]
    done = 10
    for i in (2, 6):
        w[i] = 0x35000008 | ((done - i) << 5)   # cbnz w8, +(done-i)*4
    w[9] = bl(CAVE + 9 * 4, PTP_ENABLE)
    return struct.pack('<13I', *w)


def main(src, dst):
    d = bytearray(open(src, 'rb').read())
    hook = HOOK - B
    assert struct.unpack_from('<I', d, hook)[0] == MOV_X0_X19, 'hook site differs: wrong core'
    assert struct.unpack_from('<I', d, 0x401dac28 - B)[0] == bl(0x401dac28, 0x401db984), 'poll layout differs'
    code = cave()
    assert not any(d[CAVE - B:CAVE - B + 0x80]), 'cave not empty (payload grew into it?)'
    d[CAVE - B:CAVE - B + len(code)] = code
    struct.pack_into('<I', d, hook, bl(HOOK, CAVE))
    open(dst, 'wb').write(d)
    print(f'patched {dst}: hook {HOOK:#x} -> cave {CAVE:#x} ({len(code)} B)')


if __name__ == '__main__':
    main(*sys.argv[1:3])
