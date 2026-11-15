"""Read-only-ish probe: allocate heap blocks, test each page with Fenestra core identity() (0x401d1958),
which is the exact check module sessions use (at s1e1r/w, PA==VA, attr 0xff). Frees everything."""
import struct, sys, json
sys.path.insert(0, '.')
import livectl, z8_audio_probe as zp
IDENT, ALLOC, FREE = 0x401d1958, 0x40d2c408, 0x40d2c448
t, p = livectl.connect()
call = lambda a, *x: zp.native_call(p, a, x, 10000)
out = []
ctx = call(ALLOC, 0x1000, 0)
blocks = []
try:
    for size in (0x10000, 0x40000, 0x100000, 0x200000, 0x400000):
        for pool in (0, 1):
            b = call(ALLOC, size, pool)
            if not b:
                out.append(dict(size=hex(size), pool=pool, addr=0)); continue
            blocks.append(b)
            start = (b + 0xfff) & ~0xfff; end = (b + size) & ~0xfff
            p.debug.write(ctx + 0x8c8, struct.pack('<QQ', start, end))
            bad_r = bad_w = 0; first = None; n = 0
            for va in range(start, end, 0x1000):
                n += 1
                r = call(IDENT, ctx, va, 1) & 1
                w = call(IDENT, ctx, va, 3) & 1
                if not r: bad_r += 1
                if not w: bad_w += 1
                if (not r or not w) and first is None: first = hex(va)
            out.append(dict(size=hex(size), pool=pool, addr=hex(b), pages=n, bad_read=bad_r, bad_write=bad_w, first_bad=first))
            print(out[-1], flush=True)
finally:
    for b in blocks: call(FREE, b)
    call(FREE, ctx)
print(json.dumps(out))
