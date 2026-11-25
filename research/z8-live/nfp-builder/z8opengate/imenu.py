"""Public source adapted from the pinned research implementation; regular files only."""
from z8prores.package import require
"""Factory-derived i-menu tables and display definitions. All addresses are byte VAs.

The two native templates reserve 57 rows (four 57*8 rectangle arrays and 61
16-byte groups). Only 19 are populated by stock N-RAW. New groups use existing
slots 22..24, existing generic widget IDs and their reserved native rectangles.
No new writable RAM region or guessed graphical resource ID is introduced.
"""
import struct
B = 1075183616
INIT = 42991648768
IOFF = 78721024
p32 = lambda *v: struct.pack('<' + 'I' * len(v), *v)
p64 = lambda *v: struct.pack('<' + 'Q' * len(v), *v)

def extend(stock, put):
    cur = 1082933248
    allocations = []

    def raw(a, n):
        o = a - INIT + IOFF if a >= INIT else a - B
        return stock[o:o + n]

    def write(a, data, why):
        put(B + a - INIT + IOFF if a >= INIT else a, data, why)

    def alloc(data, name):
        nonlocal cur
        a = cur
        cur = cur + len(data) + 7 & ~7
        require(cur < 1082982400, 'Unexpected firmware structure')
        put(a, bytes(data), name)
        allocations.append(dict(name=name, va=hex(a), bytes=len(data)))
        return a

    def mov16(a, value, kind):
        w = struct.unpack('<I', raw(a, 4))[0]
        require(w & 4286578688 == kind, (hex(a), hex(w)))
        put(a, p32(w & ~(65535 << 5) | (value & 65535) << 5), 'relocate i-menu constant')

    def address(lo, hi, a):
        mov16(lo, a, 3531603968)
        mov16(hi, a >> 16, 4068474880)

    def count(a, n):
        mov16(a, n, 1384120320)
    rows = []
    codes = []
    for q, table, codetable in [(1, 1118856748, 1118858116), (0, 1118857204, 1118858268)]:
        original = [list(struct.unpack('<6I', raw(table + i * 24, 24))) for i in range(19)]
        extra = []
        for j, rate in enumerate((4, 5, 6)):
            r = list(original[2 + j])
            require(r[:4] == [6, rate, 0, q], 'Unexpected firmware structure')
            r[2] = 1
            r[4] = j
            extra.append(r)
        for r in original:
            r[4] += 3
        rows.append(alloc(b''.join((p32(*r) for r in extra + original)), 'N-RAW ' + str(q) + ' rows: OG first, then all 19 factory choices'))
        c = [struct.unpack('<2I', raw(codetable + i * 8, 8))[1] for i in range(19)]
        codes.append(alloc(b''.join((p32(i, v) for i, v in enumerate(c[2:5] + c))), 'Native inhibition codes; OG uses matching 30/25/24p code'))
    high, normal = rows
    hc, nc = codes
    conditions = raw(1134675064, 76)
    cond = alloc(conditions[8:20] + conditions, '22 native availability predicates, OG first')
    option = alloc(b''.join((p32(i, i) for i in range(22))), '22 independent i-menu option indexes')
    put(1134694136, p64(option, 22), 'N-RAW option set: 22 entries')
    put(1118842932, p32(22), 'N-RAW dial wrap count')
    put(1118843032, p64(cond), 'N-RAW selected-row availability table')
    for lo, hi, addr in [(1078827716, 1078827724, normal), (1078827720, 1078827728, high), (1078872504, 1078872512, normal), (1078872508, 1078872516, high), (1078872520, 1078872532, cond), (1078827412, 1078827420, nc), (1078827416, 1078827424, hc)]:
        address(lo, hi, addr)
    count(1078872560, 22)
    count(1078827452, 22)
    configs = []
    for a, labelbase in [(42992181584, 3137), (42992184168, 3140), (42992184624, 3143)]:
        old = [list(struct.unpack('<6I', raw(a + i * 24, 24))) for i in range(19)]
        for r in old:
            r[0] += 3
        if a == 42992181584:
            prefix = [(i, 2, labelbase + i, 0, 0, 0) for i in range(3)]
        else:
            prefix = []
            for i in range(3):
                r = list(struct.unpack('<6I', raw(a + (i + 2) * 24, 24)))
                require(r[1] == 1, 'Unexpected firmware structure')
                r[0] = i
                prefix.append(r)
        cfg = prefix + old
        configs.append(alloc(b''.join((p32(*r) for r in cfg)), '22 resource selectors, OG first; native paired star/no-star icons retained'))
    title, highcfg, normalcfg = configs
    templates = []
    for group, titlewidget, firstwidget, normalwidget in [(42991997176, 42992181512, 42992182040, 42992183104), (42992052584, 42992297832, 42992297904, 42992298968)]:
        write(titlewidget + 8, p64(title, 22), 'N-RAW i-menu title: 22 localized resources')
        for base, cfg in [(firstwidget, highcfg), (normalwidget, normalcfg)]:
            for i in range(19):
                require(struct.unpack('<Q', raw(base + i * 56 + 16, 8))[0] == 19, 'Unexpected firmware structure')
                write(base + i * 56 + 8, p64(cfg, 22), 'existing N-RAW widget: 22 selectors')
        hi0 = list(struct.unpack('<7Q', raw(firstwidget, 56)))
        lo0 = list(struct.unpack('<7Q', raw(normalwidget, 56)))
        for i in range(19, 22):
            defs = []
            for original, cfg in [(hi0, highcfg), (lo0, normalcfg)]:
                r = list(original)
                r[0] += i
                r[1] = cfg
                r[2] = 22
                for k in range(3, 7):
                    r[k] += i * 8
                defs.append(alloc(p64(*r), 'N-RAW native reserved widget ' + hex(r[0])))
            records = alloc(b''.join((p64(0, v, 0, 0, 0) for v in defs)), 'N-RAW row ' + str(i) + ' widget records')
            at = group + (3 + i) * 16
            require(raw(at, 16) == bytes(16), 'Unexpected firmware structure')
            write(at, p64(records, 2), 'populate existing spare N-RAW group ' + str(3 + i))
        templates.append(dict(groups=hex(group), title_widget=hex(titlewidget), native_capacity=57, choices=22))
    return dict(order='OG 30/25/24p, then the 19 unchanged factory choices', count=22, rows={'high': hex(high), 'normal': hex(normal)}, conditions=hex(cond), options=hex(option), templates=templates, allocations=allocations, display_scope='OG title uses localized text; cells reuse exact factory rate/quality image resources and native star selection')
