"""Bind scoped native SFX calls to the linked duplex adapter; preserve B/BL."""
import hashlib
import struct

BASE = 0x40160000
BANKS = ((0x4019f020, 0x4019ff40), (0x401e2000, 0x401fcfb0),
         (0x401fd218, 0x401fe000))


def patch(data, expected, symbols, guards):
    assert hashlib.sha256(data).hexdigest() == expected, 'input SHA mismatch'
    hooks = guards['hooks']
    required = {0x40e34eb0, 0x40e34ff4, 0x40e35060, 0x40e35138,
                0x40e351fc, 0x40e3535c, 0x40e3527c, 0x40e3539c,
                0x40e35540, 0x40e355e4}
    sites = {int(h['site'], 0) for h in hooks}
    assert sites in (required, required | {0x40c7e090}) and len(hooks) == len(sites)
    image = bytearray(data)
    changes = []
    for h in hooks:
        site = int(h['site'], 0)
        start = int(h['window_address'], 0)
        window = bytes.fromhex(h['window_hex'])
        assert data[start-BASE:start-BASE+len(window)] == window, h['site']
        old = int(h['old_word'], 0)
        assert struct.unpack_from('<I', data, site-BASE)[0] == old
        opcode = old & 0xfc000000
        assert opcode in (0x14000000, 0x94000000)
        assert h['branch'] == ('BL' if opcode == 0x94000000 else 'B')
        old_delta = old & 0x3ffffff
        if old_delta & (1 << 25):
            old_delta -= 1 << 26
        assert site + old_delta*4 == int(h['original_target'], 0)
        target = symbols[h['symbol']]
        assert target % 4 == 0 and any(a <= target < b for a, b in BANKS)
        assert any(data[target-BASE:target-BASE+16]), h['symbol']
        delta = target-site
        assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27)
        new = opcode | ((delta//4) & 0x3ffffff)
        struct.pack_into('<I', image, site-BASE, new)
        changes.append(dict(site=hex(site), symbol=h['symbol'], target=hex(target),
                            before=hex(old), after=hex(new), branch=h['branch']))
    restored = bytearray(image)
    for h in hooks:
        struct.pack_into('<I', restored, int(h['site'], 0)-BASE, int(h['old_word'], 0))
    assert restored == data, 'unexpected changes outside scoped hooks'
    return bytes(image), changes
