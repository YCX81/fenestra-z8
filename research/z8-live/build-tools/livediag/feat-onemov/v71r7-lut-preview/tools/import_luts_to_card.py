#!/usr/bin/env python3
"""Batch .cube LUT validator, normalizer, 65->33 resampler, and SD-card importer
for Nikon Z8 V71.7+ (FENESTRA/LUTS/<Category>/<Name>.cube).

Matches exact V71.7 firmware constraints (v71r7_lut_abi.h, photo_catalog.c, cube.c):
  - Category folder: 1..26 chars, ASCII [A-Za-z0-9_-]
  - File stem:       1..26 chars, ASCII [A-Za-z0-9_-], extension ".cube"
  - Path budget:     "LUTS\\<cat>\\<stem>.cube" < 64 bytes
  - File size:       <= 2 MiB (PHOTO_MAX_CUBE_BYTES)
  - 3D Grid size:    2 <= LUT_3D_SIZE <= 33 (auto-resamples >33 -> 33 trilinearly;
                     converts 1D-only .cube -> 17^3 separable 3D cube so V71.7 XM6
                     Op12 RGBLUT hardware fast-path still activates!)
  - Float format:    <= 12 digits per number (cube.c parse_number limit!) -> outputs
                     clean 6-decimal format (e.g. "0.123456"), strips UTF-8 BOM &
                     unsupported keywords that cube.c rejects.

Usage:
  python3 import_luts_to_card.py <src_dir_or_zip> [<src2> ...] \
      --out <FENESTRA/LUTS dir> [--default-category Film] [--max-per-category 120] [--clean]
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import shutil
import sys
import unicodedata
import zipfile
from collections import defaultdict
from pathlib import Path

MAX_COMP_LEN = 26
MAX_GRID = 33
MAX_BYTES = 2 * 1024 * 1024


def sanitize_component(raw: str, fallback: str = "LUT") -> str:
    """Convert any directory or filename stem into 1..26 chars of [A-Za-z0-9_-]."""
    s = unicodedata.normalize("NFKD", raw)
    # Replace common separators/punctuation with underscore
    s = re.sub(r"[^A-Za-z0-9_-]+", "_", s)
    s = re.sub(r"_+", "_", s).strip("_-")
    if not s:
        h = hashlib.sha1(raw.encode("utf-8", errors="ignore")).hexdigest()[:6]
        s = f"{fallback}_{h}"
    if len(s) > MAX_COMP_LEN:
        h = hashlib.sha1(raw.encode("utf-8", errors="ignore")).hexdigest()[:4]
        s = f"{s[:MAX_COMP_LEN - 5].rstrip('_-')}_{h}"
    return s[:MAX_COMP_LEN]


def alloc_unique(base: str, used_lower: set[str]) -> str:
    """Ensure case-insensitive uniqueness on FAT32/exFAT within 26-char limit."""
    cand = base
    idx = 2
    while cand.lower() in used_lower:
        suf = f"_{idx}"
        cand = base[:MAX_COMP_LEN - len(suf)].rstrip("_-") + suf
        idx += 1
    used_lower.add(cand.lower())
    return cand


def parse_cube_text(text: str) -> dict:
    """Parse a .cube file (3D or 1D) strictly matching cube.c semantics."""
    # Strip UTF-8 BOM
    if text.startswith("\ufeff"):
        text = text[1:]

    grid_3d = 0
    grid_1d = 0
    domain_min = [0.0, 0.0, 0.0]
    domain_max = [1.0, 1.0, 1.0]
    has_min = False
    has_max = False
    title = None
    triples: list[tuple[float, float, float]] = []

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        # Strip inline comments
        hash_pos = line.find("#")
        if hash_pos >= 0:
            line = line[:hash_pos].strip()
            if not line:
                continue

        parts = line.split()
        head = parts[0].upper()

        if head == "TITLE":
            title = line[5:].strip().strip('"')
            continue
        if head == "LUT_3D_SIZE":
            if len(parts) < 2:
                raise ValueError("Invalid LUT_3D_SIZE")
            grid_3d = int(parts[1])
            continue
        if head == "LUT_1D_SIZE":
            if len(parts) < 2:
                raise ValueError("Invalid LUT_1D_SIZE")
            grid_1d = int(parts[1])
            continue
        if head == "DOMAIN_MIN" and len(parts) >= 4:
            domain_min = [float(parts[1]), float(parts[2]), float(parts[3])]
            has_min = True
            continue
        if head == "DOMAIN_MAX" and len(parts) >= 4:
            domain_max = [float(parts[1]), float(parts[2]), float(parts[3])]
            has_max = True
            continue
        if head in ("LUT_3D_INPUT_RANGE", "LUT_1D_INPUT_RANGE") and len(parts) >= 3:
            lo, hi = float(parts[1]), float(parts[2])
            domain_min = [lo, lo, lo]
            domain_max = [hi, hi, hi]
            has_min = has_max = True
            continue
        if head in ("LUT_IN_VIDEO_RANGE", "LUT_OUT_VIDEO_RANGE"):
            continue

        if len(parts) == 3:
            try:
                r, g, b = float(parts[0]), float(parts[1]), float(parts[2])
            except ValueError as exc:
                raise ValueError(f"Unrecognized line: {line[:60]}") from exc
            if not (math.isfinite(r) and math.isfinite(g) and math.isfinite(b)):
                raise ValueError("Non-finite float in LUT")
            triples.append((r, g, b))
        else:
            # Skip unknown metadata directives before data starts
            if not triples and head.isalpha():
                continue
            raise ValueError(f"Unexpected line format: {line[:60]}")

    for c in range(3):
        if domain_max[c] <= domain_min[c]:
            raise ValueError("Invalid DOMAIN_MIN >= DOMAIN_MAX")

    if grid_3d >= 2:
        expected = grid_3d ** 3
        if grid_1d > 0 and len(triples) == grid_1d + expected:
            # Shaper + 3D combined cube: take trailing 3D table only if shaper is identity,
            # else compose shaper onto 3D input coordinates.
            shaper = triples[:grid_1d]
            triples = triples[grid_1d:]
            triples = compose_shaper_into_3d(shaper, triples, grid_3d)
        elif len(triples) != expected:
            raise ValueError(f"Expected {expected} 3D entries for size {grid_3d}, got {len(triples)}")
        return {
            "kind": "3d",
            "grid": grid_3d,
            "domain_min": domain_min,
            "domain_max": domain_max,
            "has_domain": has_min or has_max,
            "title": title,
            "triples": triples,
        }

    if grid_1d >= 2:
        if len(triples) != grid_1d:
            raise ValueError(f"Expected {grid_1d} 1D entries, got {len(triples)}")
        # Convert 1D LUT to separable 17x17x17 3D cube (V71.7 compiles back to XM6 Op12 RGBLUT!)
        n = 17
        out_3d: list[tuple[float, float, float]] = []
        def sample_1d(ch: int, u: float) -> float:
            pos = max(0.0, min(1.0, u)) * (grid_1d - 1)
            i0 = int(math.floor(pos))
            i1 = min(grid_1d - 1, i0 + 1)
            t = pos - i0
            return triples[i0][ch] * (1.0 - t) + triples[i1][ch] * t

        lut_r = [sample_1d(0, i / (n - 1)) for i in range(n)]
        lut_g = [sample_1d(1, i / (n - 1)) for i in range(n)]
        lut_b = [sample_1d(2, i / (n - 1)) for i in range(n)]
        # .cube order: R fastest, then G, then B slowest
        for bi in range(n):
            bv = lut_b[bi]
            for gi in range(n):
                gv = lut_g[gi]
                for ri in range(n):
                    out_3d.append((lut_r[ri], gv, bv))
        return {
            "kind": "1d_to_3d",
            "grid": n,
            "domain_min": domain_min,
            "domain_max": domain_max,
            "has_domain": has_min or has_max,
            "title": title,
            "triples": out_3d,
        }

    raise ValueError("Missing LUT_3D_SIZE and LUT_1D_SIZE")


def sample_3d_trilinear(triples: list[tuple[float, float, float]], n: int,
                        r: float, g: float, b: float) -> tuple[float, float, float]:
    """Trilinear interpolation in standard .cube layout (R fastest, G mid, B slowest)."""
    rx = max(0.0, min(1.0, r)) * (n - 1)
    gy = max(0.0, min(1.0, g)) * (n - 1)
    bz = max(0.0, min(1.0, b)) * (n - 1)
    x0 = int(math.floor(rx)); x1 = min(n - 1, x0 + 1); tx = rx - x0
    y0 = int(math.floor(gy)); y1 = min(n - 1, y0 + 1); ty = gy - y0
    z0 = int(math.floor(bz)); z1 = min(n - 1, z0 + 1); tz = bz - z0

    def at(x: int, y: int, z: int) -> tuple[float, float, float]:
        return triples[(z * n + y) * n + x]

    c000 = at(x0, y0, z0); c100 = at(x1, y0, z0)
    c010 = at(x0, y1, z0); c110 = at(x1, y1, z0)
    c001 = at(x0, y0, z1); c101 = at(x1, y0, z1)
    c011 = at(x0, y1, z1); c111 = at(x1, y1, z1)

    res = [0.0, 0.0, 0.0]
    for ch in range(3):
        c00 = c000[ch] * (1.0 - tx) + c100[ch] * tx
        c10 = c010[ch] * (1.0 - tx) + c110[ch] * tx
        c01 = c001[ch] * (1.0 - tx) + c101[ch] * tx
        c11 = c011[ch] * (1.0 - tx) + c111[ch] * tx
        c0 = c00 * (1.0 - ty) + c10 * ty
        c1 = c01 * (1.0 - ty) + c11 * ty
        res[ch] = c0 * (1.0 - tz) + c1 * tz
    return (res[0], res[1], res[2])


def compose_shaper_into_3d(shaper: list[tuple[float, float, float]],
                           triples: list[tuple[float, float, float]],
                           n: int) -> list[tuple[float, float, float]]:
    m = len(shaper)
    def eval_shaper(ch: int, u: float) -> float:
        pos = max(0.0, min(1.0, u)) * (m - 1)
        i0 = int(math.floor(pos)); i1 = min(m - 1, i0 + 1); t = pos - i0
        return shaper[i0][ch] * (1.0 - t) + shaper[i1][ch] * t
    out_n = min(n, MAX_GRID)
    sr = [eval_shaper(0, i / (out_n - 1)) for i in range(out_n)]
    sg = [eval_shaper(1, i / (out_n - 1)) for i in range(out_n)]
    sb = [eval_shaper(2, i / (out_n - 1)) for i in range(out_n)]
    out: list[tuple[float, float, float]] = []
    for bi in range(out_n):
        for gi in range(out_n):
            for ri in range(out_n):
                out.append(sample_3d_trilinear(triples, n, sr[ri], sg[gi], sb[bi]))
    return out


def resample_3d_to_33(triples: list[tuple[float, float, float]], n: int,
                      target: int = MAX_GRID) -> list[tuple[float, float, float]]:
    """Fast trilinear resample from N^3 -> target^3 (uses numpy if installed)."""
    try:
        import numpy as np  # type: ignore
        arr = np.asarray(triples, dtype=np.float32).reshape((n, n, n, 3))
        coords = np.linspace(0.0, n - 1, target, dtype=np.float32)
        i0 = np.floor(coords).astype(np.int32)
        i1 = np.minimum(n - 1, i0 + 1)
        t = coords - i0

        # Interpolate along R (axis 2), then G (axis 1), then B (axis 0)
        tmp = arr[:, :, i0, :] * (1.0 - t[None, None, :, None]) + arr[:, :, i1, :] * t[None, None, :, None]
        tmp = tmp[:, i0, :, :] * (1.0 - t[None, :, None, None]) + tmp[:, i1, :, :] * t[None, :, None, None]
        tmp = tmp[i0, :, :, :] * (1.0 - t[:, None, None, None]) + tmp[i1, :, :, :] * t[:, None, None, None]
        flat = tmp.reshape((-1, 3))
        return [(float(r), float(g), float(b)) for r, g, b in flat]
    except Exception:
        out: list[tuple[float, float, float]] = []
        inv = 1.0 / (target - 1)
        for bi in range(target):
            b = bi * inv
            for gi in range(target):
                g = gi * inv
                for ri in range(target):
                    out.append(sample_3d_trilinear(triples, n, ri * inv, g, b))
        return out


def format_normalized_cube(parsed: dict, clean_title: str) -> bytes:
    """Emit a V71.7 cube.c-guaranteed-valid ASCII .cube file (<= 2 MiB, <= 7 digits/num)."""
    n = parsed["grid"]
    triples = parsed["triples"]
    if n > MAX_GRID:
        triples = resample_3d_to_33(triples, n, MAX_GRID)
        n = MAX_GRID

    lines = [
        f'TITLE "{clean_title}"',
        f"LUT_3D_SIZE {n}",
    ]
    if parsed["has_domain"]:
        lo = parsed["domain_min"]
        hi = parsed["domain_max"]
        lines.append(f"DOMAIN_MIN {lo[0]:.6f} {lo[1]:.6f} {lo[2]:.6f}")
        lines.append(f"DOMAIN_MAX {hi[0]:.6f} {hi[1]:.6f} {hi[2]:.6f}")

    # Clamp to [0, 1] and format cleanly to 6 decimals (7-8 chars, well under 12-digit limit)
    for r, g, b in triples:
        rc = 0.0 if r < 0.0 else (1.0 if r > 1.0 else r)
        gc = 0.0 if g < 0.0 else (1.0 if g > 1.0 else g)
        bc = 0.0 if b < 0.0 else (1.0 if b > 1.0 else b)
        lines.append(f"{rc:.6f} {gc:.6f} {bc:.6f}")

    raw = ("\n".join(lines) + "\n").encode("ascii")
    if len(raw) > MAX_BYTES:
        raise ValueError(f"Normalized .cube exceeds 2 MiB: {len(raw)} bytes")
    return raw


def iter_source_cubes(sources: list[Path], default_category: str):
    """Yield (raw_category, raw_stem, source_desc, raw_bytes) from directories or .zip files."""
    for src in sources:
        if src.is_file() and src.suffix.lower() == ".zip":
            with zipfile.ZipFile(src, "r") as zf:
                for info in sorted(zf.infolist(), key=lambda i: i.filename):
                    if info.is_dir():
                        continue
                    parts = [p for p in Path(info.filename).parts if p not in ("", ".", "__MACOSX")]
                    if not parts or any(p.startswith(".") or p == "__MACOSX" for p in parts):
                        continue
                    if not parts[-1].lower().endswith(".cube"):
                        continue
                    raw_stem = Path(parts[-1]).stem
                    raw_cat = parts[-2] if len(parts) >= 2 else default_category
                    yield raw_cat, raw_stem, f"{src.name}!{info.filename}", zf.read(info)
        elif src.is_dir():
            for p in sorted(src.rglob("*")):
                if not p.is_file() or p.suffix.lower() != ".cube":
                    continue
                if any(part.startswith(".") for part in p.relative_to(src).parts):
                    continue
                rel = p.relative_to(src)
                raw_cat = rel.parts[-2] if len(rel.parts) >= 2 else default_category
                yield raw_cat, p.stem, str(p), p.read_bytes()
        elif src.is_file() and src.suffix.lower() == ".cube":
            yield default_category, src.stem, str(src), src.read_bytes()
        else:
            print(f"[WARN] Skipping unsupported source: {src}", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate, normalize, and import .cube LUTs for Nikon Z8 V71.7+")
    ap.add_argument("sources", nargs="+", type=Path, help="Input directory, .zip archive, or .cube file(s)")
    ap.add_argument("--out", required=True, type=Path, help="Output FENESTRA/LUTS directory")
    ap.add_argument("--default-category", default="Film", help="Category name for top-level .cube files")
    ap.add_argument("--max-per-category", type=int, default=160,
                    help="Split huge single categories into chunks of N files for faster camera pagination (0=no split)")
    ap.add_argument("--clean", action="store_true", help="Remove existing output directory first")
    ap.add_argument("--manifest", type=Path, default=None, help="Optional JSON import report path")
    args = ap.parse_args()

    out_dir: Path = args.out
    if args.clean and out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # First group items by sanitized category so we can cleanly split huge categories if needed
    raw_items = list(iter_source_cubes(args.sources, args.default_category))
    by_cat: dict[str, list[tuple[str, str, bytes]]] = defaultdict(list)
    for raw_cat, raw_stem, src_desc, data in raw_items:
        cat = sanitize_component(raw_cat, fallback=args.default_category)
        by_cat[cat].append((raw_stem, src_desc, data))

    used_cats_lower: set[str] = set()
    imported = 0
    resampled = 0
    converted_1d = 0
    skipped: list[dict] = []
    categories_summary: dict[str, int] = {}

    for base_cat in sorted(by_cat.keys(), key=str.lower):
        items = by_cat[base_cat]
        limit = args.max_per_category if args.max_per_category > 0 else len(items) + 1
        chunks = [items[i:i + limit] for i in range(0, len(items), limit)] if len(items) > limit else [items]

        for chunk_idx, chunk in enumerate(chunks, start=1):
            cat_label = base_cat if len(chunks) == 1 else f"{base_cat[:MAX_COMP_LEN - 3].rstrip('_-')}_{chunk_idx:02d}"
            cat_name = alloc_unique(cat_label, used_cats_lower)
            cat_dir = out_dir / cat_name
            cat_dir.mkdir(parents=True, exist_ok=True)

            used_files_lower: set[str] = {p.stem.lower() for p in cat_dir.glob("*.cube")}
            cat_ok = 0

            for raw_stem, src_desc, raw_bytes in chunk:
                try:
                    text = raw_bytes.decode("utf-8", errors="replace")
                    parsed = parse_cube_text(text)
                    was_resampled = parsed["grid"] > MAX_GRID
                    was_1d = parsed["kind"] == "1d_to_3d"
                    stem_base = sanitize_component(raw_stem, fallback="LUT")
                    stem = alloc_unique(stem_base, used_files_lower)
                    norm_bytes = format_normalized_cube(parsed, stem)

                    # Final strict firmware path length assertion
                    rel_fw = f"LUTS\\{cat_name}\\{stem}.cube"
                    assert len(rel_fw) < 64, f"Firmware path too long: {rel_fw}"
                    assert len(norm_bytes) <= MAX_BYTES

                    (cat_dir / f"{stem}.cube").write_bytes(norm_bytes)
                    imported += 1
                    cat_ok += 1
                    if was_resampled:
                        resampled += 1
                    if was_1d:
                        converted_1d += 1
                except Exception as exc:
                    skipped.append({"source": src_desc, "error": str(exc)})

            if cat_ok > 0:
                categories_summary[cat_name] = cat_ok
            elif not any(cat_dir.iterdir()):
                cat_dir.rmdir()

    report = {
        "out_dir": str(out_dir),
        "total_scanned": len(raw_items),
        "imported": imported,
        "resampled_gt33_to_33": resampled,
        "converted_1d_to_3d17": converted_1d,
        "skipped_count": len(skipped),
        "categories": categories_summary,
        "skipped_examples": skipped[:20],
    }
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0 if imported > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
