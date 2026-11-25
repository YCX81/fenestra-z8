#!/usr/bin/env python3
"""Build two Nikon Z8 C3.11 native hif\x01 supplemental CJK fonts (CJK_EXT_1 & CJK_EXT_2).

Design:
  - Uses Microsoft YaHei Regular (msyh.ttc, 2048 UPM, clean non-overlapping CW outer /
    CCW inner TrueType quadratic contours) least-squares fitted against Nikon Z8 stock
    Font[2] (zh-CN) over all shared Han glyphs.
  - Emits standalone flags=0 hif\x01 blobs (exact Font[18] header structure) with
    ascent=1760, descent=288, advances=(2048, 1024, 1170) -> +0.0% line-height impact.
  - Multi-range glyph index keeps every range strictly <= 65535 bytes so Nikon's
    0x40884a88 u16 offset lookup works for arbitrarily large font blobs.
  - Splits cleanly into:
      CJK_EXT_1 (<= 556 KiB for proven mapped cave 0x408c6000..0x40951000)
      CJK_EXT_2 (<= 472 KiB for proven mapped cave 0x40b8a000..0x40c00000)
  - Verifies every encoded glyph with a round-trip decoder + checks zero overlap,
    strict cmap ordering, and exact byte budgets.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

BASE = 0x40160000
FONT_PTR_TABLE = 0x42DEC310
FONT_SIZE_TABLE = 0x42DEC3D0

MAX_BLOB_1 = 0x40951000 - 0x408C6000  # 569,344 B (556 KiB)
MAX_BLOB_2 = 0x40C00000 - 0x40B8A000  # 483,328 B (472 KiB)

# Glyph record flags (matching Nikon stock CJK fonts)
HAS_OUTLINE = 0x0002
FULL_WIDTH = 0x0012   # adv0 = 2048, vertical bearings present
HALF_WIDTH = 0x0092   # adv1 = 1024, vertical bearings present
ASCENT = 1760
DESCENT = 288

# Curated mandatory CJK punctuation & symbols
CJK_PUNCTUATION = (
    "—–·《》【】「」『』‘’“”：；！？，。、（）、…～"
    "％℃±×÷°′″※§→←↑↓●○◆◇■□▲△★☆"
)

# Curated Traditional Chinese UI & Photography/FilmSim/Camera Level-2 Han
EXTRA_PRIORITY_HAN = (
    # All Traditional Chinese UI characters for Live / C64 / Portrait / Fenestra / NFP
    "實況照片快門聲音開啟關閉準備短片段就緒儲存不可用卡錯誤回放按鍵"
    "豎直拍錄像錄影切換重啟恢復濾鏡槽狀態啟動讀取寫入套用運行已用異常"
    "換卡存檔代提交上限生效激活插件管理自動關機保護清除標記正常取消調試"
    "上一頁下一頁重試載入檢測存儲介面資源未加載視訊畫質幀率寬容度曲線"
    "預設系統韌體記憶語言簡繁體選擇確認編輯顯示隱藏鎖定連接網絡傳輸匯入"
    "還原統計總數餘量暫停繼續播放補償曝光對焦銳利柔和顆粒光暈暗角偏色"
    "飽和對比清晰流暢穩定專業個性層級範圍輕重細粗密疏淺深懷舊華麗極致"
    "膠卷膠片正片負片沖掃紀實風景建築街頭劇場螢幕電影復古黑白單色鮮豔"
    "標準純淨濃淡暖冷褪色反差高光陰影階調"
    # Photography / Film Simulation / Camera Brands / Color Terms (including GB2312 L2)
    "徕禄铬晕褪琥珀绯黛釉朦胧霓翡翠玛瑙珊瑚琉璃橄榄柠檬薰檀"
    "绮绚璀璨熠炫曜曦晖岚岑峻峦汐澄澈澜漪漾瀚淼沐沛沁淳熙炽煜焰煦煊"
    "嫣韵雅致臻逸悠谧茗茉莉蔷薇芙蓉荷莲菊兰桂枫桦柏桐樟橘柚柑柿杏桃梅"
    "樱莓梨蕉芒椰粟麦稻禾醇酿酌醺"
    "尼康佳能索尼富士松下奥林巴斯宾得理光适马腾龙蔡司福伦达铭匠七工匠"
    "唯卓仕永诺老蛙森养图丽美科斯莫格铁头曼富图捷信百诺神牛爱图仕南光"
    "大疆智云飞宇闪迪雷克沙致态铠侠东芝三星金士顿索尼"
    "柯达伊尔福爱克发乐凯西伯利亚电影卷反转片彩色负片黑白卷增感迫冲"
    "光圈快门感光度白平衡色温色调偏移曝光补偿测光对焦追焦眼控防抖连拍"
    "包围延时间隔定时慢动作升格降格裁切变形宽银幕变形镜头日志曲线色彩空间"
    "直出机内自定义优化校准预设槽位滤镜模拟胶片风格"
)


def extract_stock_font(main_bytes, font_idx):
    ptr = struct.unpack_from('<Q', main_bytes, FONT_PTR_TABLE - BASE + font_idx * 8)[0]
    sz = struct.unpack_from('<I', main_bytes, FONT_SIZE_TABLE - BASE + font_idx * 4)[0]
    return main_bytes[ptr - BASE:ptr - BASE + sz]


def parse_stock_hif_cmap_and_glyphs(blob):
    """Parse stock flags=6 hif\x01 font -> (cmap {cp: gid}, glyph_offsets {gid: off}, glyph0_bytes)."""
    flags = struct.unpack_from('<H', blob, 6)[0]
    idx_off, data_off = struct.unpack_from('<II', blob, 0x2C)
    # Glyph index
    nr = struct.unpack_from('<H', blob, idx_off)[0]
    gaddr = {}
    pos = idx_off + 2
    for _ in range(nr):
        first, base, cnt = struct.unpack_from('<HIH', blob, pos)
        offs = struct.unpack_from(f'<{cnt}H', blob, pos + 8)
        pos += 8 + cnt * 2
        for i, o in enumerate(offs):
            if o or i == 0:
                gaddr[first + i] = data_off + base + o
    # Primary cmap
    if flags == 6:
        sections = struct.unpack_from('<H', blob, 0x40)[0]
        primary = 0x40 + 6 + 5 * (sections - 1)
    elif flags == 0:
        primary = struct.unpack_from('<I', blob, 0x28)[0]
    else:
        raise ValueError(f"Unsupported stock hif flags={flags}")
    cmap_cnt = struct.unpack_from('<H', blob, primary)[0]
    cmap = {}
    for first, gid, run in struct.iter_unpack('<3H', blob[primary + 2:primary + 2 + 6 * cmap_cnt]):
        for i in range(run):
            cmap[first + i] = gid + i
    return cmap, gaddr


def decode_hif_glyph(raw, offset):
    """Decode a single hif\x01 glyph record -> (dict, end_offset)."""
    def take(fmt):
        nonlocal offset
        vals = struct.unpack_from(fmt, raw, offset)
        offset += struct.calcsize(fmt)
        return vals if len(vals) > 1 else vals[0]

    flags = take('<H')
    if flags & 0x0008:
        raise ValueError('Composite hif glyph unsupported')
    contours = take('<H') if (flags & HAS_OUTLINE) else 0
    adv = take('<h') if ((flags >> 7) & 3) == 3 else None
    vert = take('<h') if ((flags >> 5) & 3) == 3 else None
    w, h, xmin, ymax = take('<HHhh')
    bearings = take('<hh') if not (flags & 0x0400) else None
    extra = take('<h') if (flags & 0x0200) else None
    if not contours:
        return dict(flags=flags, contours=[], size=(w, h), x_min=xmin, y_max=ymax,
                    bearings=bearings, advance=adv, vertical=vert, extra=extra), offset
    npts = take('<H')
    ends = [take('<H' if npts > 0xFF else '<B') for _ in range(contours - 1)] + [npts - 1]
    on_curve = [(raw[offset + i // 8] >> (i % 8)) & 1 for i in range(npts)]
    offset += (npts + 7) // 8
    dcount = take('<H')
    descs = raw[offset:offset + dcount]
    offset += dcount
    x = y = 0
    flat, rem, it = [], 0, iter(descs)
    kind = 0
    for bit in on_curve:
        if not rem:
            kind = next(it)
            rem = (kind & 0x3F) + 1
        x += take('<b' if (kind & 0x80) else '<h')
        y += take('<b' if (kind & 0x40) else '<h')
        flat.append((x, y, bit))
        rem -= 1
    if rem or next(it, None) is not None:
        raise ValueError('Descriptor count mismatch')
    starts = [0] + [e + 1 for e in ends[:-1]]
    clist = [flat[s:e + 1] for s, e in zip(starts, ends)]
    return dict(flags=flags, contours=clist, size=(w, h), x_min=xmin, y_max=ymax,
                bearings=bearings, advance=adv, vertical=vert, extra=extra), offset


def encode_hif_glyph(contours, flags=FULL_WIDTH, advance=2048):
    xs = [x for c in contours for x, _, _ in c]
    ys = [y for c in contours for _, y, _ in c]
    xmin, xmax, ymin, ymax = min(xs), max(xs), min(ys), max(ys)
    w, h = xmax - xmin, ymax - ymin
    bx, by = xmin - advance // 2, ASCENT - ymax
    out = bytearray(struct.pack('<HHHHhhhh', flags, len(contours), w, h, xmin, ymax, bx, by))
    flat = [p for c in contours for p in c]
    npts = len(flat)
    out += struct.pack('<H', npts)
    end = -1
    for c in contours[:-1]:
        end += len(c)
        out += struct.pack('<H' if npts > 0xFF else '<B', end)
    bits = bytearray((npts + 7) // 8)
    for i, (_, _, on) in enumerate(flat):
        if on:
            bits[i // 8] |= 1 << (i % 8)
    out += bits
    deltas, x, y = [], 0, 0
    for px, py, _ in flat:
        deltas.append((px - x, py - y))
        x, y = px, py
    kinds = [(0x80 if -128 <= dx <= 127 else 0) | (0x40 if -128 <= dy <= 127 else 0)
             for dx, dy in deltas]
    descs, i = bytearray(), 0
    while i < len(kinds):
        j = i
        while j < len(kinds) and kinds[j] == kinds[i] and j - i < 64:
            j += 1
        descs.append(kinds[i] | (j - i - 1))
        i = j
    out += struct.pack('<H', len(descs)) + descs
    for (dx, dy), k in zip(deltas, kinds):
        out += struct.pack('<b' if k & 0x80 else '<h', dx)
        out += struct.pack('<b' if k & 0x40 else '<h', dy)
    return bytes(out)


class TtfSource:
    """Pure-Python TrueType (.ttf / .ttc) simple-outline & composite-decomposing reader."""
    def __init__(self, path, ttc_index=0):
        d = Path(path).read_bytes()
        self.d = d
        if d[:4] == b'ttcf':
            off0 = struct.unpack_from('>I', d, 12 + ttc_index * 4)[0]
        else:
            off0 = 0
        nt = struct.unpack_from('>H', d, off0 + 4)[0]
        self.tables = {}
        for i in range(nt):
            tag, _, off, ln = struct.unpack_from('>4sIII', d, off0 + 12 + i * 16)
            self.tables[tag.decode('latin1')] = (off, ln)
        self.upm = struct.unpack_from('>H', d, self.tables['head'][0] + 18)[0]
        loca_fmt = struct.unpack_from('>h', d, self.tables['head'][0] + 50)[0]
        lo, ll = self.tables['loca']
        self.glyf_off = self.tables['glyf'][0]
        if loca_fmt == 1:
            ng = (ll // 4) - 1
            self.locas = struct.unpack_from(f'>{ng + 1}I', d, lo)
        else:
            ng = (ll // 2) - 1
            self.locas = tuple(x * 2 for x in struct.unpack_from(f'>{ng + 1}H', d, lo))
        self.cmap = self._parse_cmap()

    def _parse_cmap(self):
        d = self.d
        co, _ = self.tables['cmap']
        _, nsub = struct.unpack_from('>HH', d, co)
        cmap = {}
        for want_fmt in (12, 4):
            for i in range(nsub):
                _, _, suboff = struct.unpack_from('>HHI', d, co + 4 + i * 8)
                so = co + suboff
                fmt = struct.unpack_from('>H', d, so)[0]
                if fmt != want_fmt:
                    continue
                if fmt == 12:
                    ng = struct.unpack_from('>I', d, so + 12)[0]
                    for g in range(ng):
                        sc, ec, sg = struct.unpack_from('>III', d, so + 16 + g * 12)
                        for cp in range(sc, ec + 1):
                            cmap[cp] = sg + (cp - sc)
                    return cmap
                if fmt == 4:
                    sc2 = struct.unpack_from('>H', d, so + 6)[0] // 2
                    ends = struct.unpack_from(f'>{sc2}H', d, so + 14)
                    starts = struct.unpack_from(f'>{sc2}H', d, so + 16 + 2 * sc2)
                    deltas = struct.unpack_from(f'>{sc2}h', d, so + 16 + 4 * sc2)
                    roffs = struct.unpack_from(f'>{sc2}H', d, so + 16 + 6 * sc2)
                    for k in range(sc2):
                        if starts[k] == 0xFFFF:
                            break
                        for cp in range(starts[k], ends[k] + 1):
                            if roffs[k] == 0:
                                gid = (cp + deltas[k]) & 0xFFFF
                            else:
                                idx_pos = so + 16 + 6 * sc2 + 2 * k + roffs[k] + 2 * (cp - starts[k])
                                gid = struct.unpack_from('>H', d, idx_pos)[0]
                                if gid:
                                    gid = (gid + deltas[k]) & 0xFFFF
                            if gid:
                                cmap[cp] = gid
                    return cmap
        return cmap

    def decode_gid(self, gid, depth=0):
        if depth > 4:
            return []
        d = self.d
        s, e = self.locas[gid], self.locas[gid + 1]
        if s == e:
            return []
        pos = self.glyf_off + s
        nc = struct.unpack_from('>h', d, pos)[0]
        pos += 10
        if nc > 0:
            ends = list(struct.unpack_from(f'>{nc}H', d, pos))
            pos += nc * 2
            il = struct.unpack_from('>H', d, pos)[0]
            pos += 2 + il
            npts = ends[-1] + 1
            fls = []
            while len(fls) < npts:
                f = d[pos]; pos += 1
                fls.append(f)
                if f & 8:
                    r = d[pos]; pos += 1
                    fls.extend([f] * r)
            xs, x = [], 0
            for f in fls:
                if f & 2:
                    dx = d[pos]; pos += 1
                    x += dx if (f & 16) else -dx
                elif not (f & 16):
                    dx = struct.unpack_from('>h', d, pos)[0]; pos += 2
                    x += dx
                xs.append(x)
            ys, y = [], 0
            for f in fls:
                if f & 4:
                    dy = d[pos]; pos += 1
                    y += dy if (f & 32) else -dy
                elif not (f & 32):
                    dy = struct.unpack_from('>h', d, pos)[0]; pos += 2
                    y += dy
                ys.append(y)
            pts = [(xs[i], ys[i], fls[i] & 1) for i in range(npts)]
            cs, conts = 0, []
            for ce in ends:
                c = pts[cs:ce + 1]
                if len(c) > 2:
                    conts.append(c)
                cs = ce + 1
            return conts
        # Composite glyph decomposition (mainly for rare punctuation/symbols)
        conts = []
        while True:
            fl, sub_gid = struct.unpack_from('>HH', d, pos); pos += 4
            if fl & 0x0001:  # ARG_1_AND_2_ARE_WORDS
                a1, a2 = struct.unpack_from('>hh' if (fl & 2) else '>HH', d, pos); pos += 4
            else:
                a1, a2 = struct.unpack_from('>bb' if (fl & 2) else '>BB', d, pos); pos += 2
            m00 = m11 = 1.0
            m01 = m10 = 0.0
            if fl & 0x0008:
                sc = struct.unpack_from('>h', d, pos)[0] / 16384.0; pos += 2
                m00 = m11 = sc
            elif fl & 0x0040:
                m00, m11 = [v / 16384.0 for v in struct.unpack_from('>hh', d, pos)]; pos += 4
            elif fl & 0x0080:
                m00, m01, m10, m11 = [v / 16384.0 for v in struct.unpack_from('>hhhh', d, pos)]; pos += 8
            dx, dy = (a1, a2) if (fl & 2) else (0, 0)
            for c in self.decode_gid(sub_gid, depth + 1):
                conts.append([(round(x * m00 + y * m10 + dx), round(x * m01 + y * m11 + dy), on)
                              for x, y, on in c])
            if not (fl & 0x0020):
                break
        return conts

    def contours(self, cp):
        gid = self.cmap.get(cp)
        if not gid:
            return []
        return self.decode_gid(gid)


def fit_axis(pairs):
    n = len(pairs)
    sx = sum(s for s, _ in pairs)
    sy = sum(t for _, t in pairs)
    sxx = sum(s * s for s, _ in pairs)
    sxy = sum(s * t for s, t in pairs)
    scale = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    return scale, (sy - scale * sx) / n


def fit_placement_vs_font2(ttf, blob2, cmap2, gaddr2):
    xs, ys = [], []
    for cp, gid in cmap2.items():
        if not (0x4E00 <= cp <= 0x9FFF) or gid not in gaddr2:
            continue
        g, _ = decode_hif_glyph(blob2, gaddr2[gid])
        if g['flags'] != FULL_WIDTH or not g['contours']:
            continue
        src = ttf.contours(cp)
        if not src:
            continue
        sxs = [p[0] for c in src for p in c]
        sys_ = [p[1] for c in src for p in c]
        xs += [(min(sxs), g['x_min']), (max(sxs), g['x_min'] + g['size'][0])]
        ys += [(min(sys_), g['y_max'] - g['size'][1]), (max(sys_), g['y_max'])]
    (xscale, dx), (yscale, dy) = fit_axis(xs), fit_axis(ys)
    return xscale, yscale, dx, dy, len(xs) // 2


def build_hif_blob(glyph_records, glyph0_bytes):
    """Build a complete standalone flags=0 hif\x01 font blob matching Nikon Font[18] layout.

    glyph_records: dict {codepoint (int): encoded_glyph_bytes} (all codepoints > 0, <= 0xFFFF).
    Returns complete hif\x01 bytes.
    """
    cps = sorted(glyph_records.keys())
    assert all(0 < cp <= 0xFFFF for cp in cps)

    # Assign gid = 1..len(cps), gid 0 = glyph0 (.notdef)
    cp_to_gid = {cp: i + 1 for i, cp in enumerate(cps)}

    # Build sorted cmap runs starting with (0, 0, 0) just like Font[18]
    runs = [(0, 0, 0)]
    for cp in cps:
        gid = cp_to_gid[cp]
        if len(runs) > 1 and runs[-1][0] + runs[-1][2] == cp and runs[-1][1] + runs[-1][2] == gid:
            f, g, r = runs[-1]
            runs[-1] = (f, g, r + 1)
        else:
            runs.append((cp, gid, 1))

    cmap_bytes = struct.pack('<H', len(runs)) + b''.join(struct.pack('<3H', *e) for e in runs)

    # Pack glyph records into multi-range glyph index (each range's record span <= 65535 B)
    all_records = bytearray()
    ranges = []  # (first_gid, base_offset, [u16 offsets])

    # Build ordered list of (gid, bytes): gid 0 first, then 1..N
    ordered = [(0, glyph0_bytes)] + [(i + 1, glyph_records[cp]) for i, cp in enumerate(cps)]
    cur_first = 0
    cur_base = 0
    cur_offs = []
    cur_chunk_len = 0

    for gid, rec in ordered:
        # Must start new range if offset of this glyph would exceed 65535
        if cur_offs and cur_chunk_len > 65535:
            ranges.append((cur_first, cur_base, cur_offs))
            cur_first = gid
            cur_base = len(all_records)
            cur_offs = []
            cur_chunk_len = 0
        # Also ensure within a range, i > 0 never has offset == 0 (always true since len(rec) >= 14)
        assert not cur_offs or cur_chunk_len > 0
        cur_offs.append(cur_chunk_len)
        all_records += rec
        cur_chunk_len += len(rec)

    if cur_offs:
        ranges.append((cur_first, cur_base, cur_offs))

    index_bytes = bytearray(struct.pack('<H', len(ranges)))
    for first, base, offs in ranges:
        index_bytes += struct.pack('<HIH', first, base, len(offs))
        index_bytes += struct.pack(f'<{len(offs)}H', *offs)

    cmap_offset = 0x34
    index_offset = cmap_offset + len(cmap_bytes)
    data_offset = index_offset + len(index_bytes)

    # Exact Font[18] flags=0 header with Font[2] CJK advances (2048, 1024, 1170) & metrics (1760, 288)
    header = bytearray(0x34)
    header[0:4] = b'hif\x01'
    struct.pack_into('<HH', header, 0x04, 512, 0)              # ver=0x0200, flags=0
    struct.pack_into('<hh', header, 0x08, ASCENT, DESCENT)     # asc=1760, desc=288
    struct.pack_into('<HH', header, 0x0C, 2048, 2048)          # em=2048, 2048
    struct.pack_into('<HHHH', header, 0x10, 0, 2, 0, 2)
    struct.pack_into('<3h', header, 0x18, 2048, 1024, 1170)    # adv0=2048, adv1=1024, adv2=1170
    struct.pack_into('<h', header, 0x1E, 2048)
    struct.pack_into('<hhhh', header, 0x20, 0, 0, -240, 96)
    struct.pack_into('<I', header, 0x28, cmap_offset)          # 0x34
    struct.pack_into('<I', header, 0x2C, index_offset)
    struct.pack_into('<I', header, 0x30, data_offset)

    blob = bytes(header) + cmap_bytes + bytes(index_bytes) + bytes(all_records)
    return blob, len(runs), len(ranges)


def verify_hif_blob(blob, expected_cps):
    """Independently parse and round-trip decode EVERY glyph in the generated hif\x01 blob."""
    assert blob[:4] == b'hif\x01'
    flags = struct.unpack_from('<H', blob, 6)[0]
    assert flags == 0
    asc, desc = struct.unpack_from('<hh', blob, 8)
    assert (asc, desc) == (ASCENT, DESCENT)
    cmap, gaddr = parse_stock_hif_cmap_and_glyphs(blob)
    assert set(cmap.keys()) == set(expected_cps), f"cmap mismatch: {len(cmap)} vs {len(expected_cps)}"
    assert 0 in gaddr
    g0, _ = decode_hif_glyph(blob, gaddr[0])
    assert len(g0['contours']) > 0
    for cp, gid in cmap.items():
        assert gid in gaddr, f"missing gid {gid} for U+{cp:04X}"
        g, end = decode_hif_glyph(blob, gaddr[gid])
        assert end <= len(blob)
        assert len(g['contours']) > 0, f"empty glyph for U+{cp:04X}"
        assert g['flags'] == FULL_WIDTH


def gb2312_chars():
    gb1, gb2 = [], []
    for lead in range(0xB0, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                ch = bytes([lead, trail]).decode('gb2312')
            except UnicodeDecodeError:
                continue
            if lead <= 0xD7:
                gb1.append(ch)
            else:
                gb2.append(ch)
    return gb1, gb2


def big5_common_han():
    out = []
    for lead in range(0xA4, 0xC7):
        for trail in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try:
                ch = bytes([lead, trail]).decode('big5')
            except UnicodeDecodeError:
                continue
            if 0x4E00 <= ord(ch) <= 0x9FFF:
                out.append(ch)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stock-main', type=Path, required=True)
    ap.add_argument('--ttf', type=Path, required=True)
    ap.add_argument('--out-dir', type=Path, required=True)
    args = ap.parse_args()

    stock = args.stock_main.read_bytes()
    blob2 = extract_stock_font(stock, 2)   # zh-CN
    blob0 = extract_stock_font(stock, 0)   # zh-TW
    blob18 = extract_stock_font(stock, 18) # full ASCII + symbols

    cmap2, gaddr2 = parse_stock_hif_cmap_and_glyphs(blob2)
    cmap0, _ = parse_stock_hif_cmap_and_glyphs(blob0)
    cmap18, _ = parse_stock_hif_cmap_and_glyphs(blob18)

    # Copy stock Font[2] glyph[0] (.notdef) verbatim
    _, g0_end = decode_hif_glyph(blob2, gaddr2[0])
    glyph0_bytes = blob2[gaddr2[0]:g0_end]
    print(f"Stock Font[2]: {len(cmap2)} cmap entries, glyph[0]={len(glyph0_bytes)} B")
    print(f"Stock Font[0]: {len(cmap0)} cmap entries, Font[18]: {len(cmap18)} cmap entries")

    ttf = TtfSource(args.ttf, ttc_index=0)
    xscale, yscale, dx, dy, nfit = fit_placement_vs_font2(ttf, blob2, cmap2, gaddr2)
    print(f"Fitted {args.ttf.name} vs Font[2] over {nfit} shared Han: "
          f"xscale={xscale:.5f}, yscale={yscale:.5f}, dx={dx:.2f}, dy={dy:.2f}")

    # Helper to encode one codepoint from TTF into hif\x01 record
    encoded_cache = {}
    def encode_cp(cp):
        if cp in encoded_cache:
            return encoded_cache[cp]
        conts = ttf.contours(cp)
        if not conts:
            return None
        scaled = []
        for c in conts:
            sc = [(round(x * xscale + dx), round(y * yscale + dy), on) for x, y, on in c]
            if len(sc) > 2:
                scaled.append(sc)
        if not scaled:
            return None
        rec = encode_hif_glyph(scaled, FULL_WIDTH, 2048)
        encoded_cache[cp] = rec
        return rec

    gb1, gb2 = gb2312_chars()
    big5 = big5_common_han()

    stock2_cps = set(cmap2.keys())
    stock0_cps = set(cmap0.keys())
    stock18_cps = set(cmap18.keys())

    # Build ordered priority queue of codepoints to pack into CJK_EXT_1 (<=556 KiB) and CJK_EXT_2 (<=472 KiB)
    # Tier 1: CJK punctuation not in Font[2] and not in Font[18]
    tier1 = [ord(c) for c in CJK_PUNCTUATION if ord(c) not in stock2_cps and ord(c) not in stock18_cps]
    # Tier 2: Curated mandatory Traditional UI + FilmSim/Camera Han (not in Font[2], OR in Font[2] only if needed for Font[0])
    tier2 = [ord(c) for c in EXTRA_PRIORITY_HAN if ord(c) not in stock2_cps]
    # Tier 3: ALL remaining GB2312 Level-1 Han not in Font[2] (100% GB2312 L1 coverage!)
    tier3 = [ord(c) for c in gb1 if ord(c) not in stock2_cps]
    # Tier 4: Simplified Han present in Font[2] but missing from Font[0] (cross-language JA/TC/KO completeness for GB2312 L1!)
    tier4 = [ord(c) for c in gb1 if ord(c) in stock2_cps and ord(c) not in stock0_cps]
    # Tier 5: Common Traditional Chinese Han (Big5 common) not in Font[0] and not in Font[2]
    tier5 = [ord(c) for c in big5 if ord(c) not in stock0_cps and ord(c) not in stock2_cps]
    # Tier 6: Remaining GB2312 Level-2 Han
    tier6 = [ord(c) for c in gb2 if ord(c) not in stock2_cps]

    ordered_cps = []
    seen = set()
    tier_counts = {}
    for tname, tlist in [('punct', tier1), ('priority_ui_photo', tier2), ('gb2312_l1', tier3),
                         ('cross_lang_l1', tier4), ('big5_trad', tier5), ('gb2312_l2', tier6)]:
        added = 0
        for cp in tlist:
            if cp <= 0xFFFF and cp not in seen:
                rec = encode_cp(cp)
                if rec is not None:
                    seen.add(cp)
                    ordered_cps.append((cp, tname, rec))
                    added += 1
        tier_counts[tname] = added
    print("Candidate tiers available:", tier_counts)

    # Greedily fill CJK_EXT_1 up to MAX_BLOB_1 (leaving ~4 KiB headroom for cmap/index overhead),
    # then CJK_EXT_2 up to MAX_BLOB_2!
    def pack_into_budget(items, budget):
        chosen = {}
        est_rec_bytes = len(glyph0_bytes)
        idx = 0
        # Reserve overhead: 52 B header + ~8 B/cp for cmap+index worst case (actually ~4 B/cp)
        while idx < len(items):
            cp, tname, rec = items[idx]
            # conservative estimate before exact build
            if est_rec_bytes + len(rec) + (len(chosen) + 1) * 10 + 2048 > budget:
                break
            chosen[cp] = rec
            est_rec_bytes += len(rec)
            idx += 1
        # Fine-tune: build exact blob and add more one-by-one while strictly <= budget - 64
        while idx < len(items):
            cp, tname, rec = items[idx]
            chosen[cp] = rec
            blob, _, _ = build_hif_blob(chosen, glyph0_bytes)
            if len(blob) > budget - 64:
                del chosen[cp]
                break
            idx += 1
        blob, nruns, nranges = build_hif_blob(chosen, glyph0_bytes)
        assert len(blob) <= budget
        return chosen, blob, nruns, nranges, items[idx:]

    chosen1, blob_ext1, runs1, ranges1, rest = pack_into_budget(ordered_cps, MAX_BLOB_1)
    chosen2, blob_ext2, runs2, ranges2, leftover = pack_into_budget(rest, MAX_BLOB_2)

    # Verify zero overlap between CJK_EXT_1 and CJK_EXT_2
    assert not (set(chosen1.keys()) & set(chosen2.keys()))

    # Verify EVERY mandatory character from Tier 1, 2, 3 (ALL GB2312 L1!) was included!
    covered_zh_cn = stock2_cps | stock18_cps | set(chosen1.keys()) | set(chosen2.keys())
    covered_zh_tw = stock0_cps | stock18_cps | set(chosen1.keys()) | set(chosen2.keys())

    missing_gb1 = [c for c in gb1 if ord(c) not in covered_zh_cn]
    missing_priority = [c for c in EXTRA_PRIORITY_HAN if ord(c) not in covered_zh_cn]
    missing_priority_tw = [c for c in EXTRA_PRIORITY_HAN if ord(c) not in covered_zh_tw]
    missing_punct = [c for c in CJK_PUNCTUATION if ord(c) not in covered_zh_cn and encode_cp(ord(c)) is not None]

    print(f"\n=== PACKED RESULTS ===")
    print(f"CJK_EXT_1: {len(chosen1)} glyphs, {len(blob_ext1):,} / {MAX_BLOB_1:,} B "
          f"({len(blob_ext1)/1024:.1f} KiB, free={MAX_BLOB_1-len(blob_ext1):,} B), "
          f"cmap_runs={runs1}, index_ranges={ranges1}")
    print(f"CJK_EXT_2: {len(chosen2)} glyphs, {len(blob_ext2):,} / {MAX_BLOB_2:,} B "
          f"({len(blob_ext2)/1024:.1f} KiB, free={MAX_BLOB_2-len(blob_ext2):,} B), "
          f"cmap_runs={runs2}, index_ranges={ranges2}")
    print(f"Total new glyphs in ROM: {len(chosen1) + len(chosen2):,}")
    print(f"GB2312 Level-1 coverage (zh-CN): {len(gb1) - len(missing_gb1)} / {len(gb1)} (missing={len(missing_gb1)})")
    print(f"Priority UI/Photo Han missing (zh-CN): {len(missing_priority)} {''.join(missing_priority)}")
    print(f"Priority UI/Photo Han missing (zh-TW): {len(missing_priority_tw)} {''.join(missing_priority_tw)}")
    print(f"CJK Punctuation missing: {len(missing_punct)} {''.join(missing_punct)}")

    assert len(missing_gb1) == 0, f"GB2312 L1 incomplete: {''.join(missing_gb1[:30])}"
    assert len(missing_priority) == 0, f"Priority Han incomplete (zh-CN): {''.join(missing_priority)}"
    assert len(missing_priority_tw) == 0, f"Priority Han incomplete (zh-TW): {''.join(missing_priority_tw)}"

    # Round-trip verify EVERY glyph in both blobs!
    verify_hif_blob(blob_ext1, chosen1.keys())
    verify_hif_blob(blob_ext2, chosen2.keys())
    print("Round-trip hif\x01 verification PASSED for all", len(chosen1) + len(chosen2), "glyphs!")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / 'cjk_ext_1.hif').write_bytes(blob_ext1)
    (args.out_dir / 'cjk_ext_2.hif').write_bytes(blob_ext2)
    meta = {
        'source_font': args.ttf.name,
        'fit': dict(xscale=xscale, yscale=yscale, dx=dx, dy=dy, shared_han=nfit),
        'cjk_ext_1': dict(
            address='0x408c6000', max_bytes=MAX_BLOB_1, bytes=len(blob_ext1),
            glyphs=len(chosen1), cmap_runs=runs1, index_ranges=ranges1,
        ),
        'cjk_ext_2': dict(
            address='0x40b8a000', max_bytes=MAX_BLOB_2, bytes=len(blob_ext2),
            glyphs=len(chosen2), cmap_runs=runs2, index_ranges=ranges2,
        ),
        'total_new_glyphs': len(chosen1) + len(chosen2),
        'gb2312_l1_covered_zh_cn': len(gb1) - len(missing_gb1),
        'gb2312_l1_total': len(gb1),
        'gb2312_l1_covered_zh_tw': sum(1 for c in gb1 if ord(c) in covered_zh_tw),
        'big5_common_covered_zh_tw': sum(1 for c in big5 if ord(c) in covered_zh_tw),
    }
    (args.out_dir / 'cjk_fonts_meta.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(json.dumps(meta, indent=2))


if __name__ == '__main__':
    main()
