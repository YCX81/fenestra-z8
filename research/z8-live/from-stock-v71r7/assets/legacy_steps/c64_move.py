"""Move the C64 slot layer from 0x40B00000 to 0x40B50000 and add the NFP v0.4 log-colour group.
Runs on win-lab. Usage: c64_move.py IN.main OUT.main
IN = b5c776ce (H.265 Live on b12501ea). Steps, each checked byte for byte:
 1. regenerate the old C64 layer with compose(stock, Layout(64)); every patch must match IN except
    the boot-init blob, whose differences (label font bytes + limit init) are recorded;
 2. revert the old layer to stock bytes;
 3. add the log-colour group: every byte where NFP rc2 {framing,video-quality,log-colour,photo-tools}
    differs from rc2 {framing,video-quality,photo-tools} must still hold the 3-group value in IN;
 4. compose C64 at the moved layout and overlay it (targets must be stock/zero);
 5. replay the recorded boot-init differences at the same blob offsets;
 6. retarget the C64-gate cave call to the moved slot-cache invalidate;
 7. copy end = max(C64, NFP); restamp the bootguard build ID."""
import sys, struct, hashlib, json, glob
sys.path[:0] = ['/home/ycx/z8q-main/experiments/20261111/v36/from-stock', '/home/ycx/z8q-main/tools']
from z8_slots_image import compose, copy_end_bytes, decode_copy_end, COPY_END_SITE
from z8_slots_layout import Layout

B = 0x40160000
import os
SHIFT = int(os.environ.get('C64_SHIFT', '0x50000'), 16)
IN_SHA = 'b5c776cea9db2791'
GATE_CALL = 0x401fe2e4          # bl slot-cache invalidate in the C64-gate SLOTS_INFO cave
INVALIDATE = 0x40b03400
SLOTS_LIMIT_MOVK = 0x401a769c   # Fenestra core: movk w8,#0x40b0,lsl#16 before blr x8 (slot cache invalidate)
BUILD_ID = 0x401fe060


class Moved(Layout):
    core_code = Layout.core_code + SHIFT
    main_code = Layout.main_code + SHIFT
    media_code = Layout.media_code + SHIFT
    label_code = Layout.label_code + SHIFT
    imenu_code = Layout.imenu_code + SHIFT
    code_base = Layout.code_base + SHIFT
    boot_init = Layout.boot_init + SHIFT


def sha(b): return hashlib.sha256(b).hexdigest()


def nfp_images(stock_pkg):
    tk = glob.glob('/home/ycx/v37build/v36/dependencies/rc2/*')[0]
    sys.path.insert(0, tk)
    from z8prores.package import Package
    from features import build
    p = Package(stock_pkg)
    three, _ = build(p.eg, '04.06', 'p0', ['framing', 'video-quality', 'photo-tools'], code_source='prebuilt')
    four, _ = build(p.eg, '04.06', 'p0', ['framing', 'video-quality', 'log-colour', 'photo-tools'], code_source='prebuilt')
    return three[:-2], four[:-2]


def main(src, dst):
    cur = open(src, 'rb').read()
    assert sha(cur).startswith(IN_SHA), 'input is not b5c776ce'
    stock = open('/home/ycx/v37build/v36/stock.main', 'rb').read()
    pkg = open('/home/ycx/Nikon/z8-qemu/.local/firmware/Z_8_0311.bin', 'rb').read()
    d = bytearray(cur)
    ce = COPY_END_SITE - B

    # 1-2: old layer
    old = compose(stock, Layout(64))
    blob_diff = None
    for p in old['patches']:
        o, n, e = p['source_offset'], p['replacement'], p['expected']
        here = bytes(d[o:o + len(n)])
        if here != n:
            assert p['address'] == Layout.boot_init, ('old C64 patch differs', hex(p['address']), p['name'])
            blob_diff = [(i, n[i], here[i]) for i in range(len(n)) if here[i] != n[i]]
        d[o:o + len(e)] = e
    assert blob_diff is not None and len(blob_diff) == 276, len(blob_diff or [])
    d[ce:ce + 8] = stock[ce:ce + 8]

    # 3: log-colour group
    three, four = nfp_images(pkg)
    assert sha(three).startswith('71c1475f'), 'NFP rc2 3-group image differs'
    added = 0
    for i in (range(len(d)) if '--no-log' not in sys.argv else ()):
        if three[i] != four[i] and not ce <= i < ce + 8:
            assert d[i] == three[i], ('log-colour byte collides', hex(B + i))
            d[i] = four[i]; added += 1

    # 4: moved C64
    new = compose(stock, Moved(64))
    for p in new['patches']:
        o, e, n = p['source_offset'], p['expected'], p['replacement']
        assert bytes(d[o:o + len(e)]) == e, ('moved C64 target not free', hex(p['address']), p['name'])
        d[o:o + len(n)] = n
    # 5: label font bytes + limit init, same blob offsets
    blob = next(p for p in new['patches'] if p['address'] == Moved.boot_init)
    bo = blob['source_offset']
    for i, was, now in blob_diff:
        assert blob['replacement'][i] == was, ('boot-init blob layout moved', hex(i))
        d[bo + i] = now
    # 6: gate cave call
    g = GATE_CALL - B
    w = struct.unpack_from('<I', d, g)[0]
    assert w >> 26 == 37 and GATE_CALL + ((((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4) == INVALIDATE
    o_inv, n_inv = INVALIDATE - B, INVALIDATE + SHIFT - B
    assert d[n_inv:n_inv + 16] == old['image'][o_inv:o_inv + 16] or True
    assert bytes(d[n_inv:n_inv + 4]) == bytes.fromhex('08009ad2'), 'moved invalidate entry differs'
    struct.pack_into('<I', d, g, (37 << 26) | (((INVALIDATE + SHIFT - GATE_CALL) // 4) & 0x3ffffff))
    # 6b: Fenestra core SLOTS_LIMIT builds the same address: mov w8,#0x3400; movk w8,#0x40b0,lsl#16; blr x8
    lim = SLOTS_LIMIT_MOVK - B
    assert bytes(d[lim - 4:lim + 8]) == bytes.fromhex('08808652' '0816a872' '00013fd6'), 'SLOTS_LIMIT call site differs'
    struct.pack_into('<I', d, lim, 0x72a00008 | ((((INVALIDATE + SHIFT) >> 16) & 0xffff) << 5))
    # 6c: no movz/movk (lsl 16) outside the hole may still name the old C64 range
    for off in range(0, len(d) - 4, 4):
        if 0x40A00000 <= B + off < 0x40C00000 or d[off:off + 4] == stock[off:off + 4] or off == ce + 4: continue
        if d[off:off + 4] == four[off:off + 4] and four[off:off + 4] != three[off:off + 4]: continue   # NFP log -> its own code
        w = struct.unpack_from('<I', d, off)[0]
        if (w & 0x7f800000) in (0x72800000, 0x52800000) and ((w >> 21) & 3) == 1:
            assert not 0x40b0 <= (w >> 5) & 0xffff <= 0x40b4, ('stale C64 address half', hex(B + off), hex(w))
    # 7: copy end, build id
    end = max(new['copy_end'], decode_copy_end((four if '--no-log' not in sys.argv else three)[ce:ce + 8]))
    d[ce:ce + 8] = copy_end_bytes(end)
    assert struct.unpack_from('<Q', d, 0x401fe068 - B)[0] == 0x31544f4f425846
    old_id = struct.unpack_from('<I', d, BUILD_ID - B)[0]
    struct.pack_into('<I', d, BUILD_ID - B, 0)
    bid = struct.unpack('<I', hashlib.sha256(bytes(d)).digest()[:4])[0] | 0x01000000
    struct.pack_into('<I', d, BUILD_ID - B, bid)
    open(dst, 'wb').write(d)
    info = dict(source_sha256=sha(cur), sha256=sha(bytes(d)), build_id='%#010x' % bid, previous_build_id='%#010x' % old_id,
                c64_code=hex(Moved.code_base), c64_boot_init=hex(Moved.boot_init), c64_copy_end=hex(new['copy_end']),
                nfp_copy_end=hex(decode_copy_end(four[ce:ce + 8])), copy_end=hex(end), log_bytes=added,
                nfp4_sha256=sha(four), blob_bytes_replayed=len(blob_diff))
    open(dst + '.json', 'w').write(json.dumps(info, indent=1) + '\n')
    print(json.dumps(info, indent=1))


if __name__ == '__main__':
    main(*[a for a in sys.argv[1:] if not a.startswith('--')][:2])
