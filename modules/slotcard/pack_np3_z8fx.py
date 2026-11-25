#!/usr/bin/env python3
"""Lossless Nikon .NP3 [Z8FX:...] Comment & SC*.REC (+0x39C) Z8FX Recipe Packer/Parser.

100% ABI-identical to work/filmsim/src/include/z8_filmsim_abi.h (V71.5):
  - 16-byte Z8Np3Recipe ('Z8FX' = 0x5846385A) at SC_RECORD +0x39C..0x3AB,
    covered by IEEE 802.3 CRC32 over bytes [0..11] and SC*.REC entry/file CRC32.
  - Standard Nikon .NP3 Comment TLV (tag 0x00010100, big-endian, even-aligned
    NUL-terminated UTF-8, placed before tone curve tag 0x00000002), 100%
    compatible with Z8 C3.11 stock parser 0x410adc64, NX Studio, and np3-reference.js.

Tag syntax embedded in .NP3 Comment:
  [Z8FX:G<0..6>,S<1..3>,B<0..1>,H<0..3>,L<0..1>]
  Example: [Z8FX:G2,S1,B1,H2,L1]
    G = Nikon Zf Film Grain intensity (0=Off, 1..6 -> film_style=NIKON_ZF)
    S = Nikon Zf Film Grain size (1=Small, 2=Medium, 3=Large)
    B = Zf Blur mode (0=Zf Exact, 1=Sharp)
    H = Ricoh Optical HDF level (0=Off, 1=Weak, 2=Medium, 3=Strong)
    L = Live Photo Link (0=Off, 1=On)
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Frozen Z8Np3Recipe ABI constants (z8_filmsim_abi.h)
Z8FX_MAGIC = 0x5846385A  # 'Z8FX' little-endian
Z8FX_VERSION = 1
Z8FX_FLAG_ENABLED = 1 << 0
Z8FX_BYTES = 16

Z8_STYLE_OFF = 0
Z8_STYLE_NIKON_ZF = 1

Z8_ZF_INTENSITY_MAX = 6
Z8_ZF_SIZE_MIN = 1
Z8_ZF_SIZE_MAX = 3
Z8_ZF_SIZE_DEFAULT = 2
Z8_ZF_BLUR_EXACT = 0
Z8_ZF_BLUR_SHARP = 1
Z8_HDF_MAX = 3
Z8_LIVE_MAX = 1

# Nikon .NP3 TLV constants
NP3_MAGIC = b"NCP\x00"
NP3_TAG_VERSION = 0x00000100
NP3_TAG_NAME = 0x00000200
NP3_TAG_COMMENT = 0x00010100
NP3_TAG_TONE_CURVE = 0x00000002
NP3_TAG_END = 0x00000000
NP3_MAX_COMMENT_UTF16_UNITS = 256

# SC*.REC / SC_RECORD constants (slotcard.h / slotcard.c)
SC_MAGIC = b"Z8SCARD1"
SC_FOOTER_MAGIC = 0x4B4F4353  # 'SCOK'
SC_SLOTS = 128
SC_RECORD = 1024
SC_HEADER = 592
SC_ENTRY = 8 + SC_RECORD
SC_FOOTER = 24
SC_FILE_RECORDS = 32
SC_RECIPE_OFF = 0x39C

_Z8FX_TAG_RE = re.compile(r"\[Z8FX:([^\[\]]+)\]", re.IGNORECASE)


def crc32_ieee(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


@dataclass(frozen=True)
class Z8FxRecipe:
    film_style: int = Z8_STYLE_OFF
    zf_intensity: int = 0
    zf_size: int = Z8_ZF_SIZE_DEFAULT
    zf_blur_mode: int = Z8_ZF_BLUR_SHARP
    hdf_level: int = 0
    live_link: int = 0
    enabled: bool = True

    def validate(self) -> None:
        if self.film_style not in (Z8_STYLE_OFF, Z8_STYLE_NIKON_ZF):
            raise ValueError(f"Invalid film_style: {self.film_style}")
        if not (0 <= self.zf_intensity <= Z8_ZF_INTENSITY_MAX):
            raise ValueError(f"Invalid zf_intensity (0..6): {self.zf_intensity}")
        if not (Z8_ZF_SIZE_MIN <= self.zf_size <= Z8_ZF_SIZE_MAX):
            raise ValueError(f"Invalid zf_size (1..3): {self.zf_size}")
        if self.zf_blur_mode not in (Z8_ZF_BLUR_EXACT, Z8_ZF_BLUR_SHARP):
            raise ValueError(f"Invalid zf_blur_mode (0..1): {self.zf_blur_mode}")
        if not (0 <= self.hdf_level <= Z8_HDF_MAX):
            raise ValueError(f"Invalid hdf_level (0..3): {self.hdf_level}")
        if not (0 <= self.live_link <= Z8_LIVE_MAX):
            raise ValueError(f"Invalid live_link (0..1): {self.live_link}")

    @classmethod
    def from_params(
        cls,
        grain: int = 0,
        size: int = Z8_ZF_SIZE_DEFAULT,
        blur: int = Z8_ZF_BLUR_SHARP,
        hdf: int = 0,
        live: int = 0,
        enabled: bool = True,
    ) -> "Z8FxRecipe":
        style = Z8_STYLE_NIKON_ZF if grain > 0 else Z8_STYLE_OFF
        r = cls(
            film_style=style,
            zf_intensity=grain if style == Z8_STYLE_NIKON_ZF else 0,
            zf_size=size,
            zf_blur_mode=blur,
            hdf_level=hdf,
            live_link=live,
            enabled=enabled,
        )
        r.validate()
        return r

    def to_bytes(self) -> bytes:
        self.validate()
        flags = Z8FX_FLAG_ENABLED if self.enabled else 0
        body = struct.pack(
            "<I8B",
            Z8FX_MAGIC,
            Z8FX_VERSION,
            flags,
            self.film_style,
            self.zf_intensity,
            self.zf_size,
            self.zf_blur_mode,
            self.hdf_level,
            self.live_link,
        )
        assert len(body) == 12
        return body + struct.pack("<I", crc32_ieee(body))

    @classmethod
    def from_bytes(cls, raw: bytes) -> "Z8FxRecipe":
        if len(raw) != Z8FX_BYTES:
            raise ValueError(f"Expected {Z8FX_BYTES} bytes, got {len(raw)}")
        magic, ver, flags, style, intensity, size, blur, hdf, live, crc = struct.unpack(
            "<I8BI", raw
        )
        if magic != Z8FX_MAGIC:
            raise ValueError(f"Bad Z8FX magic: 0x{magic:08x}")
        if ver != Z8FX_VERSION:
            raise ValueError(f"Unsupported Z8FX version: {ver}")
        if flags & ~Z8FX_FLAG_ENABLED:
            raise ValueError(f"Reserved Z8FX flag bits set: 0x{flags:02x}")
        expected_crc = crc32_ieee(raw[:12])
        if crc != expected_crc:
            raise ValueError(f"Z8FX CRC32 mismatch: got 0x{crc:08x}, expected 0x{expected_crc:08x}")
        r = cls(
            film_style=style,
            zf_intensity=intensity,
            zf_size=size,
            zf_blur_mode=blur,
            hdf_level=hdf,
            live_link=live,
            enabled=bool(flags & Z8FX_FLAG_ENABLED),
        )
        r.validate()
        return r

    def to_tag(self) -> str:
        self.validate()
        g = self.zf_intensity if self.film_style == Z8_STYLE_NIKON_ZF else 0
        return f"[Z8FX:G{g},S{self.zf_size},B{self.zf_blur_mode},H{self.hdf_level},L{self.live_link}]"


def parse_z8fx_tag(comment: Optional[str]) -> Optional[Z8FxRecipe]:
    """Parse [Z8FX:G2,S1,B1,H2,L1] tag from an .NP3 comment string."""
    if not comment:
        return None
    m = _Z8FX_TAG_RE.search(comment)
    if not m:
        return None
    grain = 0
    size = Z8_ZF_SIZE_DEFAULT
    blur = Z8_ZF_BLUR_SHARP
    hdf = 0
    live = 0
    seen_any = False
    for tok in m.group(1).split(","):
        tok = tok.strip()
        if not tok:
            continue
        key = tok[0].upper()
        val_str = tok[1:].strip()
        if val_str.startswith("="):
            val_str = val_str[1:].strip()
        if not val_str or not val_str.isdigit():
            raise ValueError(f"Invalid Z8FX token: {tok!r}")
        val = int(val_str)
        seen_any = True
        if key == "G":
            grain = val
        elif key == "S":
            size = val
        elif key == "B":
            blur = val
        elif key == "H":
            hdf = val
        elif key == "L":
            live = val
        else:
            raise ValueError(f"Unknown Z8FX parameter key: {key!r}")
    if not seen_any:
        raise ValueError("Empty [Z8FX:] tag")
    return Z8FxRecipe.from_params(grain=grain, size=size, blur=blur, hdf=hdf, live=live)


def _utf16_len(s: str) -> int:
    return len(s.encode("utf-16-le")) // 2


def merge_z8fx_comment(existing: Optional[str], recipe: Optional[Z8FxRecipe]) -> str:
    """Insert, replace, or remove the [Z8FX:...] tag in an .NP3 comment."""
    base = (existing or "").replace("\x00", "")
    cleaned = _Z8FX_TAG_RE.sub("", base).strip()
    cleaned = re.sub(r"\s{2,}", " ", cleaned)
    if recipe is None:
        return cleaned
    tag = recipe.to_tag()
    combined = f"{cleaned} {tag}".strip() if cleaned else tag
    if _utf16_len(combined) > NP3_MAX_COMMENT_UTF16_UNITS:
        # Trim user prefix while preserving the complete [Z8FX:...] tag
        budget = NP3_MAX_COMMENT_UTF16_UNITS - _utf16_len(tag) - 1
        if budget <= 0:
            combined = tag
        else:
            prefix = cleaned
            while prefix and _utf16_len(prefix) > budget:
                prefix = prefix[:-1]
            combined = f"{prefix.rstrip()} {tag}".strip()
    return combined


# ── Nikon .NP3 TLV Parser & Lossless Writer ─────────────────────────────────

def parse_np3_tlvs(data: bytes) -> Tuple[List[Tuple[int, bytes]], bytes]:
    """Parse Nikon .NP3 ('NCP\\0' + big-endian TLVs + 0x00000000 terminator).
    Returns (list of (tag, payload), trailing_bytes_after_end_tag)."""
    if len(data) < 8 or data[:4] != NP3_MAGIC:
        raise ValueError("Not a valid Nikon .NP3 file (missing 'NCP\\0' header)")
    tlvs: List[Tuple[int, bytes]] = []
    off = 4
    while off + 4 <= len(data):
        (tag,) = struct.unpack_from(">I", data, off)
        off += 4
        if tag == NP3_TAG_END:
            return tlvs, data[off:]
        if off + 4 > len(data):
            raise ValueError(f"Truncated TLV length for tag 0x{tag:08x} at offset {off}")
        (length,) = struct.unpack_from(">I", data, off)
        off += 4
        if off + length > len(data):
            raise ValueError(
                f"Truncated TLV payload for tag 0x{tag:08x}: need {length}, have {len(data) - off}"
            )
        tlvs.append((tag, data[off : off + length]))
        off += length
    raise ValueError("Missing 0x00000000 end marker in .NP3 file")


def serialize_np3_tlvs(tlvs: List[Tuple[int, bytes]], trailing: bytes = b"") -> bytes:
    parts = [NP3_MAGIC]
    for tag, payload in tlvs:
        parts.append(struct.pack(">II", tag, len(payload)))
        parts.append(payload)
    parts.append(struct.pack(">I", NP3_TAG_END))
    if trailing:
        parts.append(trailing)
    return b"".join(parts)


def encode_np3_comment_payload(comment: str) -> bytes:
    """Encode comment per Nikon .NP3 spec & np3-reference.js:
    UTF-8 bytes + >=1 NUL byte, zero-padded to an even byte length."""
    if "\x00" in comment:
        raise ValueError("Comment must not contain NUL characters")
    if _utf16_len(comment) > NP3_MAX_COMMENT_UTF16_UNITS:
        raise ValueError(f"Comment exceeds {NP3_MAX_COMMENT_UTF16_UNITS} UTF-16 code units")
    raw = comment.encode("utf-8")
    padded_len = (len(raw) + 1 + 1) & ~1
    return raw + b"\x00" * (padded_len - len(raw))


def decode_np3_comment_payload(payload: bytes) -> str:
    nul = payload.find(0)
    text_bytes = payload if nul < 0 else payload[:nul]
    return text_bytes.decode("utf-8")


def read_np3_info(data: bytes) -> Dict[str, object]:
    """Read version, name, comment, and parsed [Z8FX:...] recipe from .NP3 bytes."""
    tlvs, _ = parse_np3_tlvs(data)
    version = ""
    name = ""
    comment: Optional[str] = None
    for tag, val in tlvs:
        if tag == NP3_TAG_VERSION:
            version = val.rstrip(b"\x00").decode("ascii", errors="replace")
        elif tag == NP3_TAG_NAME:
            name = val.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        elif tag == NP3_TAG_COMMENT:
            comment = decode_np3_comment_payload(val)
    recipe = parse_z8fx_tag(comment)
    return {
        "version": version,
        "name": name,
        "comment": comment,
        "recipe": asdict(recipe) if recipe else None,
        "tag": recipe.to_tag() if recipe else None,
        "z8fx_hex": recipe.to_bytes().hex() if recipe else None,
        "tlv_count": len(tlvs),
        "bytes": len(data),
    }


def pack_np3_z8fx(data: bytes, recipe: Optional[Z8FxRecipe]) -> bytes:
    """Losslessly embed, update, or clear [Z8FX:...] in an .NP3 file's Comment TLV.
    All non-comment TLVs (parameters, Flexible Color, tone curve) remain bit-identical."""
    tlvs, trailing = parse_np3_tlvs(data)
    existing_comment: Optional[str] = None
    other_tlvs: List[Tuple[int, bytes]] = []
    for tag, val in tlvs:
        if tag == NP3_TAG_COMMENT:
            existing_comment = decode_np3_comment_payload(val)
        else:
            other_tlvs.append((tag, val))

    new_comment = merge_z8fx_comment(existing_comment, recipe)
    if new_comment:
        comment_tlv = (NP3_TAG_COMMENT, encode_np3_comment_payload(new_comment))
        # Place Comment TLV right before Tone Curve (0x00000002) if present,
        # matching Nikon NX Studio & np3-reference.js exact canonical order.
        insert_at = len(other_tlvs)
        for idx, (tag, _) in enumerate(other_tlvs):
            if tag == NP3_TAG_TONE_CURVE:
                insert_at = idx
                break
        other_tlvs.insert(insert_at, comment_tlv)

    return serialize_np3_tlvs(other_tlvs, trailing)


# ── SC_RECORD (1024 B) & SC*.REC File Codec ─────────────────────────────────

def read_sc_record_recipe(record: bytes) -> Optional[Z8FxRecipe]:
    """Read optional 16B Z8Np3Recipe from SC_RECORD +0x39C..0x3AB."""
    if len(record) != SC_RECORD:
        raise ValueError(f"SC_RECORD must be {SC_RECORD} bytes, got {len(record)}")
    tail = record[SC_RECIPE_OFF : SC_RECIPE_OFF + Z8FX_BYTES]
    if tail == b"\x00" * Z8FX_BYTES:
        return None
    magic = struct.unpack_from("<I", tail, 0)[0]
    if magic != Z8FX_MAGIC:
        return None
    return Z8FxRecipe.from_bytes(tail)


def write_sc_record_recipe(record: bytes, recipe: Optional[Z8FxRecipe]) -> bytes:
    """Write (or zero-clear) 16B Z8Np3Recipe at SC_RECORD +0x39C..0x3AB without
    modifying any other byte of the 1024B slot record."""
    if len(record) != SC_RECORD:
        raise ValueError(f"SC_RECORD must be {SC_RECORD} bytes, got {len(record)}")
    buf = bytearray(record)
    patch = recipe.to_bytes() if recipe is not None else (b"\x00" * Z8FX_BYTES)
    buf[SC_RECIPE_OFF : SC_RECIPE_OFF + Z8FX_BYTES] = patch
    return bytes(buf)


def sc_record_name(record: bytes) -> str:
    """Best-effort ASCII slot name extraction from SC_RECORD +0x10..0x24."""
    if len(record) != SC_RECORD:
        return ""
    raw = record[0x10:0x24].split(b"\x00", 1)[0]
    return raw.decode("ascii", errors="replace")


def parse_sc_rec_file(raw: bytes) -> Dict[str, object]:
    """Validate and parse one SC*.REC generation file (exact parity with slotcard.c)."""
    if len(raw) < SC_HEADER + SC_FOOTER:
        raise ValueError(f"SC*.REC too small: {len(raw)} bytes")
    if raw[:8] != SC_MAGIC:
        raise ValueError("Invalid SC*.REC header magic (expected 'Z8SCARD1')")
    ver, hdr_sz, gen = struct.unpack_from("<III", raw, 8)
    (store_id,) = struct.unpack_from("<Q", raw, 24)
    (count,) = struct.unpack_from("<I", raw, 36)
    sel0, sel1, proxy0, proxy1, limit = struct.unpack_from("<IIIII", raw, 40)
    (hdr_crc,) = struct.unpack_from("<I", raw, SC_HEADER - 4)
    if ver != 1 or hdr_sz != SC_HEADER or count > SC_FILE_RECORDS:
        raise ValueError(f"Invalid SC*.REC header fields: ver={ver}, hdr_sz={hdr_sz}, count={count}")
    if crc32_ieee(raw[: SC_HEADER - 4]) != hdr_crc:
        raise ValueError("SC*.REC header CRC32 mismatch")
    expected_size = SC_HEADER + count * SC_ENTRY + SC_FOOTER
    if len(raw) != expected_size:
        raise ValueError(f"SC*.REC size mismatch: got {len(raw)}, expected {expected_size}")
    foot_magic, foot_size, foot_gen, foot_store_id, file_crc = struct.unpack_from(
        "<IIIQI", raw, expected_size - SC_FOOTER
    )
    if foot_magic != SC_FOOTER_MAGIC or foot_size != expected_size or foot_gen != gen or foot_store_id != store_id:
        raise ValueError("SC*.REC footer fields mismatch")
    if crc32_ieee(raw[: expected_size - 4]) != file_crc:
        raise ValueError("SC*.REC whole-file CRC32 mismatch")

    entries = []
    for idx in range(count):
        at = SC_HEADER + idx * SC_ENTRY
        slot, pad, entry_crc = struct.unpack_from("<HHI", raw, at)
        rec = raw[at + 8 : at + 8 + SC_RECORD]
        if pad != 0 or slot >= SC_SLOTS:
            raise ValueError(f"Invalid entry header at index {idx}: slot={slot}, pad={pad}")
        if crc32_ieee(rec) != entry_crc:
            raise ValueError(f"Entry CRC32 mismatch for slot {slot}")
        recipe = read_sc_record_recipe(rec)
        entries.append(
            {
                "entry_index": idx,
                "slot": slot,
                "c_label": f"C-{(slot % 64) + 1}" + (" (Video)" if slot >= 64 else ""),
                "name": sc_record_name(rec),
                "recipe": asdict(recipe) if recipe else None,
                "tag": recipe.to_tag() if recipe else None,
            }
        )
    return {
        "gen": gen,
        "store_id": f"0x{store_id:016x}",
        "count": count,
        "selection": [sel0, sel1],
        "proxy": [proxy0, proxy1],
        "limit": limit,
        "bytes": len(raw),
        "entries": entries,
    }


def patch_sc_rec_file(raw: bytes, slot: int, recipe: Optional[Z8FxRecipe]) -> bytes:
    """Patch +0x39C..0x3AB of `slot` inside an SC*.REC file and recompute entry + file CRC32."""
    parse_sc_rec_file(raw)  # Validate input completely first
    buf = bytearray(raw)
    (count,) = struct.unpack_from("<I", buf, 36)
    found = False
    for idx in range(count):
        at = SC_HEADER + idx * SC_ENTRY
        entry_slot = struct.unpack_from("<H", buf, at)[0]
        if entry_slot == slot:
            old_rec = bytes(buf[at + 8 : at + 8 + SC_RECORD])
            new_rec = write_sc_record_recipe(old_rec, recipe)
            buf[at + 8 : at + 8 + SC_RECORD] = new_rec
            struct.pack_into("<I", buf, at + 4, crc32_ieee(new_rec))
            found = True
            break
    if not found:
        raise KeyError(f"Slot {slot} is not stored in this SC*.REC generation file")
    # Recompute header CRC32 (header untouched, safe) and whole-file footer CRC32
    struct.pack_into("<I", buf, SC_HEADER - 4, crc32_ieee(bytes(buf[: SC_HEADER - 4])))
    struct.pack_into("<I", buf, len(buf) - 4, crc32_ieee(bytes(buf[:-4])))
    out = bytes(buf)
    parse_sc_rec_file(out)  # Self-check before returning
    return out


# ── CLI ─────────────────────────────────────────────────────────────────────

def _recipe_from_args(args: argparse.Namespace) -> Optional[Z8FxRecipe]:
    if getattr(args, "clear", False):
        return None
    if getattr(args, "from_np3", None):
        info = read_np3_info(Path(args.from_np3).read_bytes())
        if info["recipe"] is None:
            raise ValueError(f"No [Z8FX:...] tag found in {args.from_np3}")
        return Z8FxRecipe(**info["recipe"])
    return Z8FxRecipe.from_params(
        grain=args.grain,
        size=args.size,
        blur=args.blur,
        hdf=args.hdf,
        live=args.live,
    )


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_nr = sub.add_parser("np3-read", help="Read .NP3 metadata and [Z8FX:...] tag")
    p_nr.add_argument("input", type=Path)

    p_np = sub.add_parser("np3-pack", help="Embed, update, or clear [Z8FX:...] in an .NP3 file")
    p_np.add_argument("input", type=Path)
    p_np.add_argument("-o", "--output", type=Path, required=True)
    p_np.add_argument("--grain", type=int, default=0, help="Zf grain intensity 0..6 (0=Off)")
    p_np.add_argument("--size", type=int, default=Z8_ZF_SIZE_DEFAULT, help="Zf grain size 1..3")
    p_np.add_argument("--blur", type=int, default=Z8_ZF_BLUR_SHARP, help="Zf blur 0=Exact, 1=Sharp")
    p_np.add_argument("--hdf", type=int, default=0, help="Ricoh HDF 0..3")
    p_np.add_argument("--live", type=int, default=0, help="Live Photo Link 0..1")
    p_np.add_argument("--clear", action="store_true", help="Remove [Z8FX:...] tag")

    p_rr = sub.add_parser("rec-read", help="Read Z8FX recipes from SC*.REC or 1024B SC_RECORD")
    p_rr.add_argument("input", type=Path)

    p_rp = sub.add_parser("rec-pack", help="Write/clear Z8FX at +0x39C in SC*.REC or 1024B SC_RECORD")
    p_rp.add_argument("input", type=Path)
    p_rp.add_argument("-o", "--output", type=Path, required=True)
    p_rp.add_argument("--slot", type=int, default=None, help="Slot index 0..127 (required for SC*.REC)")
    p_rp.add_argument("--from-np3", type=Path, default=None, help="Import [Z8FX:...] recipe from .NP3")
    p_rp.add_argument("--grain", type=int, default=0)
    p_rp.add_argument("--size", type=int, default=Z8_ZF_SIZE_DEFAULT)
    p_rp.add_argument("--blur", type=int, default=Z8_ZF_BLUR_SHARP)
    p_rp.add_argument("--hdf", type=int, default=0)
    p_rp.add_argument("--live", type=int, default=0)
    p_rp.add_argument("--clear", action="store_true")

    args = ap.parse_args(argv)
    if args.cmd == "np3-read":
        print(json.dumps(read_np3_info(args.input.read_bytes()), indent=2, ensure_ascii=False))
        return 0
    if args.cmd == "np3-pack":
        recipe = _recipe_from_args(args)
        out_bytes = pack_np3_z8fx(args.input.read_bytes(), recipe)
        args.output.write_bytes(out_bytes)
        print(json.dumps(read_np3_info(out_bytes), indent=2, ensure_ascii=False))
        return 0
    if args.cmd == "rec-read":
        raw = args.input.read_bytes()
        if len(raw) == SC_RECORD:
            rec = read_sc_record_recipe(raw)
            print(
                json.dumps(
                    {
                        "type": "SC_RECORD",
                        "name": sc_record_name(raw),
                        "recipe": asdict(rec) if rec else None,
                        "tag": rec.to_tag() if rec else None,
                    },
                    indent=2,
                )
            )
        else:
            print(json.dumps(parse_sc_rec_file(raw), indent=2))
        return 0
    if args.cmd == "rec-pack":
        raw = args.input.read_bytes()
        recipe = _recipe_from_args(args)
        if len(raw) == SC_RECORD:
            out_bytes = write_sc_record_recipe(raw, recipe)
            args.output.write_bytes(out_bytes)
            rec = read_sc_record_recipe(out_bytes)
            print(json.dumps({"type": "SC_RECORD", "recipe": asdict(rec) if rec else None}, indent=2))
        else:
            if args.slot is None:
                ap.error("--slot is required when patching an SC*.REC file")
            out_bytes = patch_sc_rec_file(raw, args.slot, recipe)
            args.output.write_bytes(out_bytes)
            print(json.dumps(parse_sc_rec_file(out_bytes), indent=2))
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
