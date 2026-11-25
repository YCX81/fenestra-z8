#!/usr/bin/env python3
"""V69 All-in-One Photo Hook Patcher (14 stock callsites, Job 2 err-85 bypass guard).

Patches the 14 stock Nikon Z8 C3.11 photo pipeline hook sites (0x40ea44d0..0x40eb02dc)
to target FilmSim EL1 symbols placed in RX bank 0x409B0000..0x409D8000.
Routes Job 2 (0x40eaf554, NEF embedded 7.04 MiB Basic preview) specifically to
`z8_np_engine19_job2_stub` so heavy-grain presets cleanly bypass Job 2 while Job 1
(Main Fine* JPEG, 29.5 MB) receives the full HDF + Fuji + Zf Grain pipeline.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

BASE = 0x40160000
FILMSIM_BANK_START = 0x409B0000
FILMSIM_BANK_END = 0x409D8000

SITES = [
    (0x40eaafc0, "fc6fbaa9", "z8_np_hook_resize",          False, "begin native filmsim same-shot resize evidence"),
    (0x40eab768, "fc6bbba9", "z8_np_hook_thumbnail",       False, "complete original thumbnail then bounded photo worker"),
    (0x40ea44d0, "f60f1df8", "z8_np_hook_cleanup",         False, "retire private group at cleanup"),
    (0x40eab3cc, "6602fa97", "z8_np_wait64_stub",          True,  "observe native resize event 80000 return"),
    (0x40eab414, "5402fa97", "z8_np_wait64_stub",          True,  "observe native resize event 40000 return"),
    (0x40eab43c, "4a02fa97", "z8_np_wait64_stub",          True,  "observe native resize event 200000 return"),
    (0x40eab48c, "7202fa97", "z8_np_wait54_stub",          True,  "observe native resize final event 40 return"),
    (0x40eab910, "d168ff97", "z8_np_thumbnail_resize_stub", True, "observe original thumbnail resize and active ROI"),
    (0x40eaeb8c, "414dff97", "z8_np_engine19_stub",        True,  "Job 1: main JPEG private local descriptor"),
    (0x40eaf554, "cf4aff97", "z8_np_engine19_job2_stub",   True,  "Job 2: NEF Basic preview (err 85 grain bypass guard)"),
    (0x40eaf7d4, "2f4aff97", "z8_np_engine19_stub",        True,  "secondary preview private local descriptor"),
    (0x40eafe58, "8e48ff97", "z8_np_engine21_stub",        True,  "medium JPEG private local descriptor"),
    (0x40eb00ec, "e947ff97", "z8_np_engine21_stub",        True,  "small JPEG private local descriptor"),
    (0x40eb02dc, "6d47ff97", "z8_np_engine19_stub",        True,  "thumbnail JPEG private local descriptor"),
]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def encode_branch(at: int, target: int, link: bool) -> bytes:
    delta = target - at
    if (delta & 3) or not (-(1 << 27) <= delta < (1 << 27)):
        raise ValueError(f"Out-of-range branch {at:#x} -> {target:#x} (delta={delta:#x})")
    word = (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x03FFFFFF)
    return struct.pack("<I", word)


def load_symbols(path: Path, nm_tool: str = "llvm-nm") -> dict[str, int]:
    if path.suffix == ".json":
        raw = json.loads(path.read_text())
        src = raw.get("symbols", raw)
        out: dict[str, int] = {}
        for k, v in src.items():
            if isinstance(v, int):
                out[k] = v
            elif isinstance(v, str) and v.startswith("0x"):
                out[k] = int(v, 16)
            elif isinstance(v, dict):
                val = v.get("value", v.get("addr", v.get("va")))
                if isinstance(val, int):
                    out[k] = val
                elif isinstance(val, str):
                    out[k] = int(val, 16)
        return out

    # Parse ELF directly via llvm-nm / nm
    text = subprocess.check_output([nm_tool, "--defined-only", str(path)], text=True)
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            out[parts[2]] = int(parts[0], 16)
    return out


def compute_patches(
    original_main: bytes,
    symbols: dict[str, int],
    *,
    expected_base_sha256: str | None = None,
    require_bank: bool = True,
) -> list[dict]:
    if len(original_main) != 81807709:
        raise ValueError(f"Unexpected MAIN length: {len(original_main)}")
    if expected_base_sha256 and sha256(original_main) != expected_base_sha256:
        raise ValueError(
            f"Base MAIN SHA-256 mismatch: expected {expected_base_sha256}, got {sha256(original_main)}"
        )

    patches = []
    for at, expected_hex, symbol, link, reason in SITES:
        off = at - BASE
        actual_hex = original_main[off:off + 4].hex()
        if actual_hex != expected_hex:
            raise ValueError(
                f"Photo hook site {at:#x} ({symbol}) original byte mismatch: "
                f"expected {expected_hex}, got {actual_hex}"
            )
        if symbol not in symbols:
            raise KeyError(f"Missing required FilmSim photo hook symbol: {symbol}")
        target = symbols[symbol]
        if require_bank and not (FILMSIM_BANK_START <= target < FILMSIM_BANK_END):
            raise ValueError(
                f"Symbol {symbol} ({target:#x}) outside FilmSim bank "
                f"[{FILMSIM_BANK_START:#x}, {FILMSIM_BANK_END:#x})"
            )
        rep = encode_branch(at, target, link)
        patches.append({
            "address": hex(at),
            "size": 4,
            "instruction": "BL" if link else "B",
            "symbol": symbol,
            "target": hex(target),
            "expected": expected_hex,
            "replacement": rep.hex(),
            "purpose": reason,
        })
    return patches


def apply(
    original_main: bytes,
    symbols: dict[str, int],
    *,
    expected_base_sha256: str | None = None,
    require_bank: bool = True,
) -> tuple[bytes, list[dict]]:
    patches = compute_patches(
        original_main, symbols,
        expected_base_sha256=expected_base_sha256,
        require_bank=require_bank,
    )
    img = bytearray(original_main)
    for p in patches:
        off = int(p["address"], 16) - BASE
        img[off:off + 4] = bytes.fromhex(p["replacement"])
    out = bytes(img)

    # Zero unexplained byte check
    allowed = {int(p["address"], 16) - BASE + i for p in patches for i in range(4)}
    diffs = [i for i, (a, b) in enumerate(zip(original_main, out)) if a != b]
    unexplained = [BASE + i for i in diffs if i not in allowed]
    if unexplained:
        raise AssertionError(f"Unexplained diffs: {[hex(x) for x in unexplained[:8]]}")
    return out, patches


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--main", type=Path, required=True, help="Input MAIN image")
    ap.add_argument("--symbols", type=Path, required=True, help="Symbol JSON or linked ELF")
    ap.add_argument("--expected-base-sha256", default=None)
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--no-bank-check", action="store_true")
    ap.add_argument("--out", type=Path, help="Output patched MAIN image (omit for dry-run)")
    ap.add_argument("--out-json", type=Path, help="Optional JSON receipt output")
    args = ap.parse_args()

    orig = args.main.read_bytes()
    syms = load_symbols(args.symbols, nm_tool=args.nm)
    patched, patches = apply(
        orig, syms,
        expected_base_sha256=args.expected_base_sha256,
        require_bank=not args.no_bank_check,
    )
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(patched)

    receipt = {
        "schema": "v69.filmsim-photo-hooks/v1",
        "base_sha256": sha256(orig),
        "output_sha256": sha256(patched),
        "hooks_patched": len(patches),
        "changed_bytes": sum(1 for a, b in zip(orig, patched) if a != b),
        "unexplained_bytes": 0,
        "job2_site": "0x40eaf554",
        "job2_symbol": "z8_np_engine19_job2_stub",
        "patches": patches,
    }
    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({
        "hooks_patched": receipt["hooks_patched"],
        "changed_bytes": receipt["changed_bytes"],
        "unexplained_bytes": 0,
        "job2_symbol": receipt["job2_symbol"],
        "output_sha256": receipt["output_sha256"],
    }, indent=2))


if __name__ == "__main__":
    main()
