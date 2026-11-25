"""Deterministic V71.7-r7 compressed delta overlay and FENESTRA asset exporter.

Uses only Python stdlib modules included in macos/runtime.json.
"""
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import zlib

HERE = Path(__file__).resolve().parent
FEATURE_ID = 'live-filmsim'
PROFILE_ID = 'z8-c311-v71r7-live-filmsim'
EG_BYTES = 81807711
MAGIC = b'Z8LF0001'

VERSION_STR_AT = 0x0009B136     # 5 B: NN.NN
BUILD_ID_AT = 0x0009E060        # 4 B: Bootguard build_id (LE u32)
CODE_VERSION_AT = 0x00EB29FC    # 12 B: public_words(version)
RUNTIME_ID_AT = 0x014EAA90      # 8 B: NN.NN.tag
TRAILER_ID_AT = EG_BYTES - 11   # 8 B: NN.NN.tag
CRC16_AT = EG_BYTES - 2         # 2 B: big-endian CRC-16/CCITT-XMODEM

DYNAMIC_SITES = (
    (VERSION_STR_AT, VERSION_STR_AT + 5),
    (BUILD_ID_AT, BUILD_ID_AT + 4),
    (CODE_VERSION_AT, CODE_VERSION_AT + 12),
    (RUNTIME_ID_AT, RUNTIME_ID_AT + 8),
    (TRAILER_ID_AT, TRAILER_ID_AT + 8),
    (CRC16_AT, EG_BYTES),
)


def _need(ok, message):
    if not ok:
        raise ValueError(message)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def public_words(version):
    _need(bool(re.fullmatch(r'[0-9]{2}\.[0-9]{2}', version)),
          'Version must be two digits, a dot and two digits, e.g. 04.06')
    raw = version.encode('ascii')
    return struct.pack(
        '<3I',
        0x52800008 | (int.from_bytes(raw[:2], 'little') << 5),
        0x72A00008 | (int.from_bytes(raw[2:4], 'little') << 5),
        0x52800009 | (raw[4] << 5),
    )


def load_meta():
    meta = json.loads((HERE / 'overlay.json').read_text(encoding='utf-8'))
    _need(meta.get('schema') == 1 and meta.get('id') == PROFILE_ID,
          'Unsupported z8livefilmsim overlay metadata')
    return meta


def load_spans(meta=None):
    meta = load_meta() if meta is None else meta
    comp_path = HERE / meta['overlay_file']
    comp = comp_path.read_bytes()
    _need(len(comp) == meta['overlay_bytes'] and _sha(comp) == meta['overlay_sha256'],
          'z8livefilmsim overlay.zlib digest mismatch')
    raw = zlib.decompress(comp)
    _need(len(raw) == meta['raw_bytes'] and _sha(raw) == meta['raw_sha256'],
          'z8livefilmsim decompressed overlay digest mismatch')
    _need(raw[:8] == MAGIC, 'Invalid z8livefilmsim overlay magic')
    (count,) = struct.unpack_from('<I', raw, 8)
    _need(count == meta['span_count'], 'z8livefilmsim span count mismatch')
    pos = 12
    spans = []
    prev_end = 0
    for _ in range(count):
        off, length = struct.unpack_from('<II', raw, pos)
        pos += 8
        chunk = raw[pos:pos + length]
        pos += length
        _need(len(chunk) == length and prev_end <= off and off + length <= EG_BYTES,
              'Invalid z8livefilmsim overlay span bounds')
        spans.append((off, chunk))
        prev_end = off + length
    _need(pos == len(raw), 'Trailing bytes in z8livefilmsim overlay')
    return spans


def compute_build_id(main_bytes):
    """Compute Bootguard build_id: u32_le(sha256(main_with_zeroed_build_id)[:4]) | 0x01000000."""
    _need(len(main_bytes) == EG_BYTES - 2, 'Invalid main payload length for build_id')
    h = hashlib.sha256()
    h.update(memoryview(main_bytes)[:BUILD_ID_AT])
    h.update(b'\x00\x00\x00\x00')
    h.update(memoryview(main_bytes)[BUILD_ID_AT + 4:])
    return struct.unpack_from('<I', h.digest(), 0)[0] | 0x01000000


def verify_fenestra_assets(meta=None):
    """Verify all 7 packaged FENESTRA files match overlay.json sizes and SHA-256 digests."""
    meta = load_meta() if meta is None else meta
    src_dir = HERE / 'FENESTRA'
    _need(src_dir.is_dir(), 'Missing z8livefilmsim/FENESTRA asset directory')
    expected = meta['fenestra']
    _need(len(expected) == 7, 'Expected 7 FENESTRA assets in overlay.json')
    verified = {}
    for name in sorted(expected):
        spec = expected[name]
        path = src_dir / name
        _need(path.is_file(), f'Missing FENESTRA asset: {name}')
        data = path.read_bytes()
        _need(len(data) == spec['bytes'] and _sha(data) == spec['sha256'],
              f'FENESTRA asset digest mismatch: {name}')
        verified[name] = dict(bytes=len(data), sha256=spec['sha256'])
    return verified


def export_fenestra(output_dir):
    """Copy all 7 verified FENESTRA files into <output_dir>/FENESTRA and verify readback."""
    meta = load_meta()
    verified = verify_fenestra_assets(meta)
    src_dir = HERE / 'FENESTRA'
    dst_dir = Path(output_dir) / 'FENESTRA'
    dst_dir.mkdir(parents=False, exist_ok=False)
    exported = []
    for name in sorted(verified):
        spec = verified[name]
        dst = dst_dir / name
        with dst.open('xb') as f:
            shutil.copyfileobj((src_dir / name).open('rb'), f)
            f.flush()
            os.fsync(f.fileno())
        readback = dst.read_bytes()
        _need(len(readback) == spec['bytes'] and _sha(readback) == spec['sha256'],
              f'FENESTRA output readback mismatch: {name}')
        exported.append(dict(name=name, bytes=spec['bytes'], sha256=spec['sha256']))
    return exported


def apply_overlay(eg, version, tag):
    """Apply the verified V71.7-r7 overlay onto stock C3.11 EG and stamp identity/build_id/CRC."""
    meta = load_meta()
    _need(len(eg) == EG_BYTES and _sha(eg) == meta['stock_eg_sha256'],
          'Incompatible input: exact original C3.11 EG required')
    ver_words = public_words(version)
    _need(bool(re.fullmatch(r'[a-z][0-9]', tag)), 'Tag must be a lowercase letter and digit')
    fenestra = verify_fenestra_assets(meta)
    spans = load_spans(meta)

    out = bytearray(eg)
    for off, chunk in spans:
        out[off:off + len(chunk)] = chunk

    ident = f'{version}.{tag}'.encode('ascii')
    out[CODE_VERSION_AT:CODE_VERSION_AT + 12] = ver_words
    out[RUNTIME_ID_AT:RUNTIME_ID_AT + 8] = ident
    out[TRAILER_ID_AT:TRAILER_ID_AT + 8] = ident
    out[VERSION_STR_AT:VERSION_STR_AT + 5] = version.encode('ascii')

    # Restamp Bootguard build_id over main (eg[:-2]) with build_id field zeroed.
    out[BUILD_ID_AT:BUILD_ID_AT + 4] = b'\x00\x00\x00\x00'
    build_id = compute_build_id(memoryview(out)[:-2])
    out[BUILD_ID_AT:BUILD_ID_AT + 4] = struct.pack('<I', build_id)

    # Recompute EG CRC-16/CCITT-XMODEM trailer.
    out[CRC16_AT:EG_BYTES] = binascii.crc_hqx(memoryview(out)[:-2], 0).to_bytes(2, 'big')
    result = bytes(out)
    _need(binascii.crc_hqx(result, 0) == 0, 'Invalid final V71.7-r7 payload CRC')

    # Verify all bytes outside declared spans + dynamic sites are untouched.
    all_windows = [(off, off + len(chunk)) for off, chunk in spans] + list(DYNAMIC_SITES)
    end = 0
    for lo, hi in sorted(all_windows):
        _need(end <= lo and result[end:lo] == eg[end:lo], 'Change outside declared V71.7-r7 windows')
        end = hi
    _need(result[end:] == eg[end:], 'Unlisted final change in V71.7-r7 overlay')

    info = dict(
        profile_id=PROFILE_ID,
        version=meta['version'],
        includes_relocated_nfp_v051=True,
        build_id=f'0x{build_id:08x}',
        overlay_sha256=meta['overlay_sha256'],
        span_count=len(spans),
        total_patch_bytes=meta['total_patch_bytes'],
        fenestra_files=fenestra,
    )
    return result, info
