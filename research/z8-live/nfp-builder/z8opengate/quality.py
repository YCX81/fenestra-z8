"""Public source adapted from the pinned research implementation; regular files only."""
from z8prores.package import require
import math, struct, hashlib, re
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from .a64x import Block
BASE, DATA_OFF, DATA_VA = (1075183616, 78721024, 42991648768)
VERSION, RUNTIME = (15411708, 21932688)
DSQ, DS_ID = (43014013584, 83890785)
FILETYPE, NRAW_FILETYPE = (43014012948, 6)
TABLE_DIR, BLOB_DIR, LANGUAGES, STRINGS = (51496352, 59223896, 36, 3136)
ID_FINE, ID_BASIC = (STRINGS + 1, STRINGS + 2)
STATE_TABLE, STATE_SIZE, STATE_NRAW = (79690944, 96, 269)
STOCK_CALLBACKS, STOCK_PAGE = (42992569768, 42992408416)
NRAW_CALLBACKS = (1079429368, 1079429460, 1077617168, 1079429464, 1079429472)
NRAW_OK = 1079429476
ITEM_HIGH, ITEM_NORMAL, ITEM_FINE, ITEM_BASIC = (12569, 12570, 13753, 13754)
ITEM_BY_VALUE = (ITEM_NORMAL, ITEM_HIGH, ITEM_FINE, ITEM_BASIC)
F_GUI_GET, F_GET, F_SET = (1077855128, 1091395272, 1077855132)
F_ENTRY_GUARD, F_SET_CURSOR, F_SET_MARK = (1077600504, 1077608520, 1077608632)
RAW_RECORDS, RAW_COUNT, RAW_HALF = (1118518416, 42, 21)
RAW_INDEX = 1087498016
DISPLAY = (b'N-RAW NORM', b'N-RAW HIGH', b'N-RAW FINE', b'N-RAW BASIC')
LIST_TEXT = {2: b'FINE', 3: b'BASIC'}
MAGIC = 5132595
MEDIA_REC = 43022460960
CURRENT = 43024430404
F_RAW_FLAG, F_STAR = (1080806708, 1080806352)
F_CODEC_CLASS, F_RECORD = (1081829720, 1082507676)
FROM_RECORDING, FROM_FILE = (1080823212, 1080819216)
TIER_BYTE, TIER_MARK = (27, 160)
LINK_CLASS = {2: 3, 3: 4}
SHIPPED = ('0451', 'q3')
Q3_UNNAMED_VERSIONS = ('04.35', '04.47', '04.49')
ALIAS = 43209719808 - 1075838976
SITES = {'list': (1079455008, 956314623, 'strb wzr,[sp,#0xc]: video menu list, row 3118h value text for values other than 0/1'), 'selector': (1089002376, 3108215049, 'ldr w9,[x8,#0x398]: Tenc rate selector, N-RAW quality of the image info'), 'rate': (1087497740, 2432701409, 'add x1,sp,#4: RAW branch of the movie bit-rate record lookup'), 'label': (1080896768, 3107986440, 'ldr w8,[x0,#0x1c]: NCTG tag 24h writer, N-RAW case'), 'derived_snapshot': (1089703592, 2483251090, 'bl 41012CF0: normalize only the capture capability snapshot after its complete table read'), 'derived_update': (1089726348, 3104012896, 'str w0,[x19,#0x378]: normalize the capture snapshot single-item update for 05001261h'), 'gui_get': (F_GUI_GET, 338929356, 'b 410D5EC8: GUI property get; 05001261h reads Fine as High and Basic as Normal'), 'reset': (1092111848, 1895827071, 'cmp w19,#1: native text restore boundary, clear the stash word'), 'play': (1080351072, 1384491465, 'mov w9,#0x2d4e: playback detail page, N-RAW codec name'), 'link': (1080897724, 3107986440, 'ldr w8,[x0,#0x1c]: NCTG tag 1015h writer, RAW partner quality class (N-RAW file type)'), 'attr': (1080820624, 2550133353, 'bl 406BCD34: DCF attribute builder, last field (RAW flag); then the tier byte'), 'codec': (1081710032, 2483057890, 'bl 407B6958: playback movie block, codec class of the shown file; then the stash word')}
LINKED = ('derived_snapshot', 'attr', 'codec')
RAW_GET_SITES = (1079454960,)
PTP_SITES = (1082397220, 1082397300, 1082397452, 1082397580)
PTP_GET = 1082483948
DERIVED = dict(describe=dict(source='05001261h in the capture capability snapshot', slot='snapshot+0x378 (index 222 of A0292A624)', mapping={'0': 0, '1': 1, '2': 1, '3': 0}, batch='40F38EA8 -> 41012CF0', single='40F3E78C', scope='Stored DS value stays 0..3; capture consumers see Fine as High and Basic as Normal.'))
GAP = (7731440, 11141120)
CODE_AT, DATA_AT, GUI_AT, TEXT_AT, TABLES_AT, STASH_AT = (7731456, 7737344, 7741440, 7749632, 7864320, 8323072)
LOADER_END_SITE, LOADER_END = (16928140, 1082915056)
SECTION_VA, SECTION_ROOM = (1075838976, 10485760)
CODE_AT, DATA_AT, GUI_AT, TEXT_AT, TABLES_AT, STASH_AT = [x - BASE for x in (1083510784, 1083518976, 1083523072, 1083531264, 1083531264, 1083506688)]
ID_FINE, ID_BASIC = (3140, 3141)
del SITES['reset']

def va(offset):
    return BASE + offset

def off(address):
    return address - DATA_VA + DATA_OFF if address >= DATA_VA else address - BASE

def sha(b):
    return hashlib.sha256(b).hexdigest()

class Image:

    def __init__(self, data):
        self.old, self.new, self.edits = (bytes(data), bytearray(data), [])

    def put(self, offset, data, purpose, expect_zero=False, expect=None):
        old = self.old[offset:offset + len(data)]
        if expect_zero and any(old):
            raise ValueError(f'{purpose}: {offset:#x} is not free')
        if expect is not None and old != expect:
            raise ValueError(f'{purpose}: unexpected stock bytes at {offset:#x}')
        for e in self.edits:
            if offset < e['end'] and e['offset'] < offset + len(data):
                raise ValueError(f"{purpose}: overlaps {e['purpose']}")
        self.new[offset:offset + len(data)] = data
        self.edits.append(dict(offset=offset, end=offset + len(data), purpose=purpose, old=old, new=bytes(data)))

    def u32(self, o):
        return struct.unpack_from('<I', self.old, o)[0]

    def u64(self, o):
        return struct.unpack_from('<Q', self.old, o)[0]

def string_tables(img):
    tables = [img.u64(TABLE_DIR + 8 * k) - BASE for k in range(LANGUAGES)]
    blobs = [img.u64(BLOB_DIR + 8 * k) - BASE for k in range(LANGUAGES)]
    return (tables, blobs)

def stock_string(img, tables, blobs, k, i):
    a, b = struct.unpack_from('<2i', img.old, tables[k] + 4 * i)
    return img.old[blobs[k] + a:blobs[k] + b].decode('utf-16le').rstrip('\x00')

def tier_names(img, tables, blobs, k):

    def grade(i):
        return re.sub('\\s*JPEG\\s*,?\\s*', ' ', stock_string(img, tables, blobs, k, i).replace('\u200f', '')).strip(' ,')
    fine, basic = (grade(762), grade(764))
    if k in (6, 11, 34):
        fine, basic = ('Fine', 'Basic')
    if k == 20:
        fine = 'İ' + fine[1:]
    return (fine[:1].upper() + fine[1:], basic[:1].upper() + basic[1:])

def rate_tables(img):
    base = off(RAW_RECORDS)
    recs = [struct.unpack_from('<IIIIQ', img.old, base + 24 * i) for i in range(RAW_COUNT)]
    fine, basic = ([list(r) for r in recs], [list(r) for r in recs])
    rows = []
    for i in range(RAW_HALF):
        h, n = (recs[i], recs[i + RAW_HALF])
        if h[:4] != n[:4]:
            raise ValueError(f'record {i} and {i + RAW_HALF} are not the same size and rate')
        w, ht, fps = (h[0] & 65535, h[0] >> 16, h[1] / h[2])
        if h[4] == 0 or n[4] == 0:
            if h[4] or n[4]:
                raise ValueError(f'record pair {i} half empty')
            rows.append(dict(index=i, width=w, height=ht, fps=round(fps, 3), unsupported=True))
            continue
        high_bpp = 2.5 if (w, ht) == (8268, 4652) and fps > 49 else 3.0
        pixel_rate = w * ht * fps
        over_h, over_n = (h[4] - high_bpp * pixel_rate, n[4] - 1.5 * pixel_rate)
        if not (0 < over_h < 0.05 * h[4] and 0 < over_n < 0.1 * n[4]):
            raise ValueError(f'record pair {i}: stock rates are not budget plus a small overhead')
        over = max(over_h, over_n)
        f = math.ceil((2.0 * pixel_rate + over) / 1000000.0) * 1000000
        b = math.ceil((1.0 * pixel_rate + over) / 1000000.0) * 1000000
        if not (n[4] < f < h[4] and b < n[4]):
            raise ValueError(f'record pair {i}: new rates out of order')
        fine[i][4], basic[i + RAW_HALF][4] = (f, b)
        rows.append(dict(index=i, width=w, height=ht, fps=round(fps, 3), high_bpp=high_bpp, high=h[4], normal=n[4], fine=f, basic=b, overhead_high=round(over_h), overhead_normal=round(over_n)))
    pack = lambda t: b''.join((struct.pack('<IIIIQ', *r) for r in t))
    return (pack(fine), pack(basic), rows)

def code(cs, data, versions):
    """data: {name: absolute address} of the constants placed at DATA_AT; versions: see unnamed_versions()."""
    blk = Block(va(CODE_AT))
    stash = va(STASH_AT) + ALIAS

    def ds_quality(reg):
        blk.mov_x64(reg, DSQ)
        blk.ldr_w(reg, reg, 0)
    blk.label('entry')
    blk.stp_pre(19, 30, -32)
    blk.str_x(20, 31, 16)
    blk.mov_x(19, 0)
    blk.mov_w(0, DS_ID & 65535)
    blk.movk_w16(0, DS_ID >> 16)
    blk.bl(F_GET)
    blk.cmp_wi(0, 3)
    blk.b_cond('hi', 'entry_default')
    blk.adr(8, data['item_by_value'])
    blk.ldr_w_idx(20, 8, 0)
    blk.b('entry_go')
    blk.label('entry_default')
    blk.mov_w(20, ITEM_HIGH)
    blk.label('entry_go')
    blk.bl(F_ENTRY_GUARD)
    blk.cmp_wi(0, 1)
    blk.b_cond('eq', 'entry_mark')
    blk.mov_x(0, 19)
    blk.mov_wr(1, 20)
    blk.bl(F_SET_CURSOR)
    blk.label('entry_mark')
    blk.mov_x(0, 19)
    blk.mov_wr(1, 20)
    blk.mov_w(2, 1)
    blk.ldr_x(20, 31, 16)
    blk.ldp_post(19, 30, 32)
    blk.b(F_SET_MARK)
    blk.label('ok')
    blk.ldr_x(9, 0, 0)
    blk.mov_w(8, 44)
    blk.ldr_x(10, 9, 80)
    blk.ldrh_w(11, 9, 88)
    blk.ldrh_w(9, 9, 50)
    blk.ldr_x(10, 10, 16)
    blk.add_x(9, 9, 11)
    blk.madd_x(8, 9, 8, 10)
    blk.ldr_w(8, 8, 24)
    blk.mov_w(1, 1)
    for item, value, nxt in ((ITEM_NORMAL, 0, 'ok_1'), (ITEM_FINE, 2, 'ok_2'), (ITEM_BASIC, 3, 'ok_3')):
        blk.mov_w(9, item)
        blk.cmp_w(8, 9)
        blk.b_cond('ne', nxt)
        blk.mov_w(1, value)
        blk.label(nxt)
    blk.mov_w(0, DS_ID & 65535)
    blk.movk_w16(0, DS_ID >> 16)
    blk.b(F_SET)
    site = SITES['list'][0]
    blk.label('list')
    blk.cmp_wi(0, 2)
    blk.b_cond('eq', 'list_fine')
    blk.cmp_wi(0, 3)
    blk.b_cond('eq', 'list_basic')
    blk.strb_w(31, 31, 12)
    blk.str_w(31, 31, 8)
    blk.b(site + 8)
    for name in ('fine', 'basic'):
        blk.label('list_' + name)
        blk.mov_w(21, 76)
        blk.adr(0, data['list_' + name])
        blk.b(site + 16)
    site = SITES['selector'][0]
    blk.label('selector')
    ds_quality(10)
    blk.cmp_wi(10, 2)
    blk.b_cond('eq', 'selector_fine')
    blk.cmp_wi(10, 3)
    blk.b_cond('eq', 'selector_basic')
    blk.ldr_w(9, 8, 920)
    blk.b(site + 4)
    blk.label('selector_fine')
    blk.mov_w(8, 1)
    blk.b(1089002436)
    blk.label('selector_basic')
    blk.mov_w(8, 0)
    blk.b(1089002436)
    site = SITES['rate'][0]
    blk.label('rate')
    blk.mov_x64(10, FILETYPE)
    blk.ldr_w(10, 10, 0)
    blk.cmp_wi(10, NRAW_FILETYPE)
    blk.b_cond('ne', 'rate_stock')
    ds_quality(10)
    blk.cmp_wi(10, 2)
    blk.b_cond('lo', 'rate_stock')
    blk.cmp_wi(10, 3)
    blk.b_cond('hi', 'rate_stock')
    blk.sub_sp(32)
    blk.ldp_x(11, 12, 0, 0)
    blk.stp_x(11, 12, 31, 0)
    blk.cmp_wi(10, 2)
    blk.cset_w(11, 'ne')
    blk.str_w(11, 31, 12)
    blk.str_w(10, 31, 16)
    blk.add_xi(0, 31, 0)
    blk.add_xi(1, 31, 20)
    blk.bl(RAW_INDEX)
    blk.ldr_w(9, 31, 20)
    blk.ldr_w(10, 31, 16)
    blk.add_sp(32)
    blk.cmp_wi(10, 2)
    blk.b_cond('ne', 'rate_basic')
    blk.adr(10, data['fine_table'])
    blk.b('rate_go')
    blk.label('rate_basic')
    blk.adr(10, data['basic_table'])
    blk.label('rate_go')
    blk.mov_w(8, 24)
    blk.b(1087497808)
    blk.label('rate_stock')
    blk.add_xi(1, 31, 4)
    blk.b(site + 4)
    site = SITES['label'][0]
    blk.label('label')
    ds_quality(9)
    blk.cmp_wi(9, 2)
    blk.b_cond('ne', 'label_1')
    blk.b(1080896744)
    blk.label('label_1')
    blk.cmp_wi(9, 3)
    blk.b_cond('ne', 'label_2')
    blk.b(1080896776)
    blk.label('label_2')
    blk.ldr_w(8, 0, 28)
    blk.b(site + 4)
    blk.label('ptp')
    blk.stp_pre(29, 30, -16)
    blk.bl(PTP_GET)
    blk.cmp_wi(0, 2)
    blk.b_cond('lo', 'ptp_done')
    blk.cmp_wi(0, 3)
    blk.b_cond('hi', 'ptp_done')
    blk.cmp_wi(0, 2)
    blk.cset_w(0, 'eq')
    blk.label('ptp_done')
    blk.ldp_post(29, 30, 16)
    blk.ret()
    blk.label('gui_get')
    blk.mov_w(9, DS_ID & 65535)
    blk.movk_w16(9, DS_ID >> 16)
    blk.cmp_w(0, 9)
    blk.b_cond('eq', 'gui_get_nraw')
    blk.b(F_GET)
    blk.label('gui_get_nraw')
    blk.stp_pre(29, 30, -16)
    blk.bl(F_GET)
    blk.cmp_wi(0, 2)
    blk.b_cond('ne', 'gui_get_basic')
    blk.mov_w(0, 1)
    blk.b('gui_get_done')
    blk.label('gui_get_basic')
    blk.cmp_wi(0, 3)
    blk.b_cond('ne', 'gui_get_done')
    blk.mov_w(0, 0)
    blk.label('gui_get_done')
    blk.ldp_post(29, 30, 16)
    blk.ret()
    site = SITES['link'][0]
    blk.label('link')
    ds_quality(9)
    blk.cmp_wi(9, 2)
    blk.b_cond('ne', 'link_basic')
    blk.mov_w(8, LINK_CLASS[2])
    blk.b(1080897752)
    blk.label('link_basic')
    blk.cmp_wi(9, 3)
    blk.b_cond('ne', 'link_stock')
    blk.mov_w(8, LINK_CLASS[3])
    blk.b(1080897752)
    blk.label('link_stock')
    blk.ldr_w(8, 0, 28)
    blk.b(site + 4)
    blk.label('attr')
    blk.stp_pre(19, 30, -32)
    blk.stp(20, 21, 16)
    blk.mov_x(19, 0)
    blk.bl(F_RAW_FLAG)
    blk.ldr_w(8, 31, 32 + 72)
    blk.cmp_wi(8, 49)
    blk.b_cond('ne', 'attr_done')
    blk.ldr_w(20, 31, 32 + 52)
    blk.ldr_x(21, 31, 32 + 104)
    blk.cmp_wi(20, LINK_CLASS[2])
    blk.b_cond('ne', 'attr_class4')
    blk.mov_x(0, 19)
    blk.mov_w(1, 1)
    blk.bl(F_STAR)
    blk.mov_w(9, 2)
    blk.b('attr_store')
    blk.label('attr_class4')
    blk.cmp_wi(20, LINK_CLASS[3])
    blk.b_cond('ne', 'attr_class12')
    blk.mov_w(9, 3)
    blk.b('attr_store')
    blk.label('attr_class12')
    blk.sub_wi(8, 20, 1)
    blk.cmp_wi(8, 1)
    blk.b_cond('hi', 'attr_done')
    blk.mov_x64(10, FROM_RECORDING)
    blk.cmp_x(21, 10)
    blk.b_cond('ne', 'attr_file')
    ds_quality(9)
    blk.cmp_wi(9, 3)
    blk.b_cond('hi', 'attr_done')
    blk.sub_wi(10, 9, 1)
    blk.cmp_wi(10, 1)
    blk.cset_w(10, 'hi')
    blk.add_wi(10, 10, 1)
    blk.cmp_w(10, 20)
    blk.b_cond('ne', 'attr_done')
    blk.b('attr_store')
    blk.label('attr_file')
    blk.mov_x64(10, FROM_FILE)
    blk.cmp_x(21, 10)
    blk.b_cond('ne', 'attr_done')
    blk.mov_x64(10, MEDIA_REC)
    blk.ldrh_w(11, 10, 250)
    blk.cmp_wi(11, 1)
    blk.b_cond('ne', 'attr_done')
    if versions:
        blk.ldr_w(11, 10, 44)
        blk.mov_w(12, 25942)
        blk.movk_w16(12, 11890)
        blk.cmp_w(11, 12)
        blk.b_cond('ne', 'attr_named')
        blk.ldrb_w(13, 10, 53)
        blk.cbnz_w(13, 'attr_named')
        blk.ldrb_w(13, 10, 52)
        blk.ldr_w(11, 10, 48)
        groups = {}
        for version in versions:
            groups[version[:4]] = groups.get(version[:4], '') + version[4]
        for major, digits in groups.items():
            nxt = 'attr_v' + major.replace('.', '')
            blk.mov_w(12, int.from_bytes(major[:2].encode(), 'little'))
            blk.movk_w16(12, int.from_bytes(major[2:].encode(), 'little'))
            blk.cmp_w(11, 12)
            blk.b_cond('ne', nxt)
            for d in digits:
                blk.cmp_wi(13, ord(d))
                blk.b_cond('eq', 'attr_done')
            blk.label(nxt)
    blk.label('attr_named')
    blk.cmp_wi(20, 1)
    blk.cset_w(9, 'eq')
    blk.label('attr_store')
    blk.add_wi(9, 9, TIER_MARK)
    blk.strb_w(9, 19, TIER_BYTE)
    blk.label('attr_done')
    blk.ldp(20, 21, 16)
    blk.ldp_post(19, 30, 32)
    blk.ret()
    blk.label('codec')
    blk.stp_pre(19, 30, -32)
    blk.stp(20, 21, 16)
    blk.mov_x(19, 0)
    blk.mov_x(20, 1)
    blk.bl(F_CODEC_CLASS)
    blk.mov_wr(21, 0)
    blk.mov_x(9, 31)
    blk.cmp_wi(21, 6)
    blk.b_cond('ne', 'codec_store')
    blk.cbz_x(20, 'codec_store')
    blk.mov_x(0, 20)
    blk.bl(F_RECORD)
    blk.ldrb_w(10, 0, TIER_BYTE)
    blk.mov_x(9, 31)
    blk.lsr_wi(11, 10, 2)
    blk.cmp_wi(11, TIER_MARK >> 2)
    blk.b_cond('ne', 'codec_store')
    blk.ldr_w(12, 19, 0)
    blk.mov_x64(9, MAGIC << 40)
    blk.add_x(9, 9, 10, lsl=32)
    blk.add_x(9, 9, 12)
    blk.label('codec_store')
    blk.mov_x64(10, stash)
    blk.str_x(9, 10, 0)
    blk.mov_wr(0, 21)
    blk.ldp(20, 21, 16)
    blk.ldp_post(19, 30, 32)
    blk.ret()
    blk.label('play')
    blk.mov_x64(8, stash)
    blk.ldr_x(8, 8, 0)
    blk.lsr_xi(9, 8, 40)
    blk.mov_w(10, MAGIC & 65535)
    blk.movk_w16(10, MAGIC >> 16)
    blk.cmp_w(9, 10)
    blk.b_cond('ne', 'play_stock')
    blk.mov_x64(11, CURRENT)
    blk.ldr_w(11, 11, 0)
    blk.cmp_w(8, 11)
    blk.b_cond('ne', 'play_stock')
    blk.lsl_xi(10, 8, 24)
    blk.lsr_xi(10, 10, 56)
    blk.lsr_wi(11, 10, 2)
    blk.cmp_wi(11, TIER_MARK >> 2)
    blk.b_cond('ne', 'play_stock')
    blk.and_w3(10, 10)
    blk.adr(11, data['display'])
    blk.add_x(11, 11, 10, lsl=4)
    blk.ldr_x(12, 11, 0)
    blk.str_x(12, 31, 280)
    blk.ldr_w(12, 11, 8)
    blk.str_w(12, 31, 288)
    blk.ldrh_w(12, 11, 12)
    blk.strh_w(12, 31, 292)
    blk.b(1080348888)
    blk.label('play_stock')
    blk.mov_w(9, 11598)
    blk.b(1080351076)
    blk.label('reset')
    blk.cmp_wi(19, 1)
    blk.b_cond('ne', 'reset_done')
    blk.stp_pre(8, 9, -16)
    blk.mov_x64(8, stash)
    blk.stp_x(31, 31, 8, 0)
    blk.ldp_post(8, 9, 16)
    blk.label('reset_done')
    blk.cmp_wi(19, 1)
    blk.b(1092111852)
    blk.label('derived_snapshot')
    blk.stp_pre(19, 30, -16)
    blk.mov_x(19, 1)
    blk.bl(1090596080)
    blk.ldr_w(8, 19, 888)
    blk.cmp_wi(8, 2)
    blk.b_cond('eq', 'derived_snapshot_fine')
    blk.cmp_wi(8, 3)
    blk.b_cond('ne', 'derived_snapshot_done')
    blk.mov_w(8, 0)
    blk.b('derived_snapshot_store')
    blk.label('derived_snapshot_fine')
    blk.mov_w(8, 1)
    blk.label('derived_snapshot_store')
    blk.str_w(8, 19, 888)
    blk.label('derived_snapshot_done')
    blk.ldp_post(19, 30, 16)
    blk.ret()
    blk.label('derived_update')
    blk.mov_wr(8, 0)
    blk.cmp_wi(8, 2)
    blk.b_cond('eq', 'derived_update_fine')
    blk.cmp_wi(8, 3)
    blk.b_cond('ne', 'derived_update_store')
    blk.mov_w(8, 0)
    blk.b('derived_update_store')
    blk.label('derived_update_fine')
    blk.mov_w(8, 1)
    blk.label('derived_update_store')
    blk.str_w(8, 19, 888)
    blk.b(1089726352)
    return blk

def menu(img, ids, blk):
    rec = STATE_TABLE + STATE_SIZE * STATE_NRAW
    state = struct.unpack_from('<HHI11Q', img.old, rec)
    if state[:2] != (2, STATE_NRAW) or state[7] != STOCK_CALLBACKS or state[12] != STOCK_PAGE:
        raise ValueError('stock N-RAW quality state differs')
    callbacks = struct.unpack_from('<5Q', img.old, off(STOCK_CALLBACKS))
    if callbacks != NRAW_CALLBACKS:
        raise ValueError('stock N-RAW quality callbacks differ')
    page = bytearray(img.old[off(STOCK_PAGE):off(STOCK_PAGE) + 80])
    layout, rows_va, evptr_va = struct.unpack_from('<3Q', page, 8)
    if struct.unpack_from('<H', page, 32)[0] != 2:
        raise ValueError('stock page row count differs')
    stock_rows = [struct.unpack_from('<11I', img.old, off(rows_va) + 44 * i) for i in range(2)]
    if [(r[0], r[6]) for r in stock_rows] != [(862, ITEM_HIGH), (863, ITEM_NORMAL)]:
        raise ValueError('stock option rows differ')
    if stock_rows[0][1:6] + stock_rows[0][7:] != stock_rows[1][1:6] + stock_rows[1][7:]:
        raise ValueError('the two stock option rows differ in more than label and item')
    template = stock_rows[0]
    rows = [stock_rows[0], (ids['fine_id'], *template[1:6], ITEM_FINE, *template[7:]), stock_rows[1], (ids['basic_id'], *template[1:6], ITEM_BASIC, *template[7:])]
    rows_bytes = b''.join((struct.pack('<11I', *r) for r in rows))
    pointers = struct.unpack_from('<2Q', img.old, off(evptr_va))
    tables = []
    for ptr in pointers:
        recs = []
        for i in range(32):
            r = struct.unpack_from('<4Q', img.old, off(ptr) + 32 * i)
            recs.append(r)
            if r[0] == 955:
                break
        tables.append(recs)
    if tables[0] != tables[1] or sum((r[1] == NRAW_OK for r in tables[0])) != 3:
        raise ValueError('stock transition tables differ from the expected pair')
    ok = blk.labels['ok']
    table = b''.join((struct.pack('<4Q', e, ok if a == NRAW_OK else a, g, t) for e, a, g, t in tables[0]))
    at = GUI_AT
    rows_at = at
    at += len(rows_bytes)
    at = at + 7 & ~7
    tables_at = at
    at += 4 * len(table)
    evptr_at = at
    at += 4 * 8
    callbacks_at = at
    at += 5 * 8
    page_at = at
    at += 80
    if at > TEXT_AT:
        raise ValueError('GUI area too small')
    struct.pack_into('<2Q', page, 16, va(rows_at), va(evptr_at))
    struct.pack_into('<H', page, 32, 4)
    img.put(rows_at, rows_bytes, 'N-RAW quality sub-page rows: High, Fine, Normal, Basic', expect_zero=True)
    img.put(tables_at, table * 4, 'sub-page transition tables, one per row (stock table, OK action -> new OK)', expect_zero=True)
    img.put(evptr_at, struct.pack('<4Q', *(va(tables_at + len(table) * i) for i in range(4))), 'sub-page transition table pointers', expect_zero=True)
    img.put(callbacks_at, struct.pack('<5Q', blk.labels['entry'], *NRAW_CALLBACKS[1:]), 'sub-page state callbacks (new entry, stock others)', expect_zero=True)
    img.put(page_at, bytes(page), 'sub-page page record (stock layout, four rows)', expect_zero=True)
    img.put(rec + 40, struct.pack('<Q', va(callbacks_at)), 'state 10Dh: callbacks -> four-row sub-page', expect=struct.pack('<Q', STOCK_CALLBACKS))
    img.put(rec + 80, struct.pack('<Q', va(page_at)), 'state 10Dh: page record -> four-row sub-page', expect=struct.pack('<Q', STOCK_PAGE))
    return dict(rows=hex(va(rows_at)), tables=hex(va(tables_at)), pointers=hex(va(evptr_at)), callbacks=hex(va(callbacks_at)), page=hex(va(page_at)), layout=hex(layout), row_items=[hex(r[6]) for r in rows], row_labels=[hex(r[0]) for r in rows])

def branch(src, dst, link=False):
    return struct.pack('<I', (2483027968 if link else 335544320) | dst - src >> 2 & 67108863)

def hooks(img, blk):
    done = {}
    for name, (site, word, meaning) in SITES.items():
        if img.u32(site - BASE) != word:
            raise ValueError(f'{name}: stock word at {site:#x} differs')
        img.put(site - BASE, branch(site, blk.labels[name], link=name in LINKED), f'hook {name}: {meaning}', expect=struct.pack('<I', word))
        done[name] = dict(site=hex(site), stock_word=hex(word), target=hex(blk.labels[name]))
    for site in RAW_GET_SITES:
        img.put(site - BASE, branch(site, F_GET, link=True), 'exact 05001261h value for the menu row text (bypass the GUI projection)', expect=branch(site, F_GUI_GET, link=True))
        done[f'raw_get_{site:x}'] = dict(site=hex(site), stock=f'bl {F_GUI_GET:#x}', target=hex(F_GET))
    for site in PTP_SITES:
        stock = branch(site, PTP_GET, link=True)
        img.put(site - BASE, branch(site, blk.labels['ptp'], link=True), 'PTP property read of 05001261h: report Fine as High, Basic as Normal', expect=stock)
        done[f'ptp_{site:x}'] = dict(site=hex(site), stock=f'bl {PTP_GET:#x}', target=hex(blk.labels['ptp']))
    return done
