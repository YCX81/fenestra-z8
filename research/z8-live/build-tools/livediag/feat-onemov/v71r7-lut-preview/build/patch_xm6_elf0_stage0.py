#!/usr/bin/env python3
"""Stage 0 standalone patch module for V71.6-XM6:
In-place zero-shift overlay patcher & bit-accurate ARM64 loader (0x40c92194) +
hardware SRAM copy (0x40c92434) verifier for Nikon Z8 C3.11 EXPEED 7 CEVA-XM6
Core 0 firmware (`z8_xm6_elf0.elf` embedded at `main.main` 0x0151caa0..0x015c5bc4).

Injects the monolithic merged CEVA-XM6 `coexist_nikondma` image (FilmGrain Slot 39/40
+ Ricoh Optical HDF v2 Slot 41, zero old HDF v1 dead code) using ONLY 2 new PT_LOAD
segments:
  - PHDR[14]: .text.z8fx   (R-X, EXPEED 7 16B-reversed, VMA 0x418c4580, <= 158,336 B)
  - PHDR[15]: .rodata.z8fx (R--, normal little-endian,  VMA 0x0003bc40, <= 2,048 B)
and updates `g_NikonDspFrameworkFunctionTable[39..41]` (`0x01528214..0x0152821f`),
while keeping `z8_xm6_elf0.elf` exact size `692,516 B (0x000a9124)` unchanged,
preserving the 12 zero-padding bytes before `ELF1 @ 0x015c5bd0`, and leaving
`ELF1`, `ELF2`, and all stock OEM PMSS / DRAM / IDM regions 100% untouched.

Zero third-party dependencies (Python 3 stdlib only).

Public API for `stage0_build_base.py` and `audit_whole_image.py`:
    apply(image: bytearray, payload_dir: Path | None = None) -> dict
    verify(image: bytearray | bytes, meta: dict) -> dict
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Z8 C3.11 main.main & CEVA-XM6 ELF0/ELF1/ELF2 Hardware Constants
# ---------------------------------------------------------------------------
BASE = 0x40160000
IMAGE_SIZE = 81807709
BUILD_ID_ADDR = 0x401FE060

ELF0_MAIN_OFFSET = 0x0151CAA0
ELF0_MAIN_VA = BASE + ELF0_MAIN_OFFSET  # 0x4167CAA0
ELF0_EXACT_SIZE = 0x000A9124            # 692,516 bytes
ELF0_MAIN_END = ELF0_MAIN_OFFSET + ELF0_EXACT_SIZE  # 0x015C5BC4

ELF0_GAP_OFFSET = 0x015C5BC4
ELF0_GAP_SIZE = 12                      # 0x015C5BC4..0x015C5BD0 (12 zero bytes before ELF1)

ELF1_MAIN_OFFSET = 0x015C5BD0
ELF1_EXACT_SIZE = 0x0006F544            # 456,004 bytes
ELF2_MAIN_OFFSET = 0x01635120
ELF2_EXACT_SIZE = 0x0057BD08            # 5,750,024 bytes

# Verified stock Z8 C3.11 SHA-256 fingerprints
ELF0_STOCK_SHA256 = "b3eebaa20c2afd3c3fffaba563d8b88e989fd3ab3cf920c6966c60f750281f97"
ELF1_STOCK_SHA256 = "de961025902457d6dab9cbb17eb832f9e441c1d685867b109bf354a8b81af2f4"
ELF2_STOCK_SHA256 = "c7787ceda602a291dea500b778aefa682a20a7c6345363d33a3b24fa3bfd041f"
STOCK_BASE_PHDRS_SHA256 = "9115c3afa9c03104ce90ba70a0deae66e685336e4881dcdd4a23853f417414b1"

# Verified stock OEM loaded memory fingerprints after 0x40c92194 + 0x40c92434
STOCK_PMSS_SHA256 = "cf84d5cca43da73670319c252c32fae2cdbee42ac62d46b877cf500947e0b44e"
STOCK_DRAM_TEXT_SHA256 = "58a47973089e880b06a7cde0c81429fbd3c56f88afe5f370050921ae8756f52e"
STOCK_DATA_EXT_SHA256 = "df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119"
STOCK_IDM_PRE_RO_MASK_SHA256 = "5291fb69af8630e86ef5296c5bdf359f6d08dbc208814996b65772349c137c0b"
STOCK_IDM_STACK_SHA256 = "f3cc103136423a57975750907ebc1d367e2985ac6338976d4d5a439f50323f4a"

# ELF32 / CEVA-XM6 Core 0 Memory Redlines
ELF_MACHINE_CEVA = 0x00F5
PT_LOAD = 1
PF_X = 0x1
PF_W = 0x2
PF_R = 0x4

STOCK_PHNUM = 14
EXPECTED_PATCHED_PHNUM = 16             # 14 stock + PHDR[14] .text.z8fx + PHDR[15] .rodata.z8fx
LEGACY_PHDR_OFFSET = 0x000034
LEGACY_PHDR_BYTES = STOCK_PHNUM * 32    # 448 B (0x000034..0x0001F4)
ELF_TAIL_PAYLOAD_START = 0x0820A0       # 32B aligned, strictly after PHDR[11] (0x082080..0x082084)

PMSS_SIZE = 0x10000                     # 64 KiB
IDM_SIZE = 0x40000                      # 256 KiB
IDM_DATA_VMA = 0x00038000
IDM_DATA_FILE_OFF = 0x00A220
IDM_RODATA_MIN = 0x0003BC40             # End of stock .bss
IDM_RODATA_MAX_BUDGET = 2048            # V71.6 <= 2 KiB cap
IDM_STACK_START = 0x0003D000            # Start of __STACK_SECT (hard ceiling)

EXT_TEXT_MIN = 0x418C4580               # End of stock PHDR[9] (.text)
EXT_TEXT_MAX_BUDGET = 158336            # 0x418c4580..0x418eb000
EXT_WORKBUF_START = 0x41930000          # Start of FW_EXT_WORKBUF / _ZL17m_XmDmaWriteQueue

FUNC_TABLE_IDM_VMA = 0x000394B8
FUNC_TABLE_SLOTS = 64
NOP_FUNCTION_VMA = 0x418BBDC0
SLOT_39_IDM_VMA = FUNC_TABLE_IDM_VMA + 39 * 4  # 0x00039554 (elf0+0x00b774, main+0x01528214)
SLOT_40_IDM_VMA = FUNC_TABLE_IDM_VMA + 40 * 4  # 0x00039558 (elf0+0x00b778, main+0x01528218)
SLOT_41_IDM_VMA = FUNC_TABLE_IDM_VMA + 41 * 4  # 0x0003955c (elf0+0x00b77c, main+0x0152821c)
SLOT_39_ELF0_OFF = IDM_DATA_FILE_OFF + (SLOT_39_IDM_VMA - IDM_DATA_VMA)  # 0x00B774
SLOT_41_ELF0_END = SLOT_39_ELF0_OFF + 12                                 # 0x00B780

# ARM64 Loader 0x40c92194 & Hardware SRAM Copy 0x40c92434 Constants
DSP_POOL_BASE = 0x0A01800000
DSP_POOL_END = 0x0A02800000
CORE0_PMSS_STAGE = DSP_POOL_BASE + 0x000000
CORE0_IDM_STAGE = DSP_POOL_BASE + 0x010000
IOMMU_WIN60_DSP_BASE = 0x40000000
IOMMU_WIN60_ARM_BASE = 0x0A00000000
IOMMU_WIN60_SIZE = 0x40000000           # 1 GiB

DEFAULT_PAYLOAD_DIR = Path(__file__).resolve().parent / "xm6_elf0" / "coexist_nikondma"


def _sha(data: bytes | bytearray | memoryview) -> str:
    return hashlib.sha256(data).hexdigest()


def _align_up(val: int, align: int) -> int:
    return (val + align - 1) & ~(align - 1)


def _align_down(val: int, align: int) -> int:
    return val & ~(align - 1)


def _bswap16_chunks(data: bytes) -> bytes:
    if len(data) % 16 != 0:
        raise ValueError(f"Executable payload length {len(data)} is not a multiple of 16 bytes")
    return b"".join(data[i : i + 16][::-1] for i in range(0, len(data), 16))


def _diff_runs(a: bytes | bytearray, b: bytes | bytearray, step: int = 4096) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    i, n = 0, len(a)
    while i < n:
        if a[i : i + step] == b[i : i + step]:
            i += step
            continue
        end = min(n, i + step)
        while i < end:
            if a[i] == b[i]:
                i += 1
                continue
            s = i
            while i < n and a[i] != b[i]:
                i += 1
            out.append((s, i))
    return out


def _merge_intervals(intervals: list[tuple[int, int]]) -> list[tuple[int, int]]:
    ivs = sorted((s, e) for s, e in intervals if e > s)
    merged: list[tuple[int, int]] = []
    for s, e in ivs:
        if merged and s <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], e))
        else:
            merged.append((s, e))
    return merged


def _subtract_runs(runs: list[tuple[int, int]], covered: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged = _merge_intervals(covered)
    rem: list[tuple[int, int]] = []
    mi = 0
    for rs, re_ in runs:
        pos = rs
        while mi < len(merged) and merged[mi][1] <= pos:
            mi += 1
        k = mi
        while pos < re_:
            if k >= len(merged) or merged[k][0] >= re_:
                rem.append((pos, re_))
                break
            if merged[k][0] > pos:
                rem.append((pos, merged[k][0]))
            pos = max(pos, merged[k][1])
            k += 1
    return rem


def _parse_elf32(elf_bytes: bytes | bytearray) -> tuple[dict[str, int], list[dict[str, int]]]:
    if len(elf_bytes) < 52 or bytes(elf_bytes[:4]) != b"\x7fELF":
        raise ValueError("Invalid ELF magic")
    if elf_bytes[4] != 1 or elf_bytes[5] != 1:
        raise ValueError("Expected ELF32 little-endian")
    (
        e_type,
        e_machine,
        e_version,
        e_entry,
        e_phoff,
        e_shoff,
        e_flags,
        e_ehsize,
        e_phentsize,
        e_phnum,
        e_shentsize,
        e_shnum,
        e_shstrndx,
    ) = struct.unpack_from("<HHIIIIIHHHHHH", elf_bytes, 16)
    if e_machine != ELF_MACHINE_CEVA:
        raise ValueError(f"Expected e_machine=0x{ELF_MACHINE_CEVA:04x}, got 0x{e_machine:04x}")
    if e_phentsize != 32:
        raise ValueError(f"Expected e_phentsize=32, got {e_phentsize}")
    hdr = {
        "e_type": e_type,
        "e_machine": e_machine,
        "e_version": e_version,
        "e_entry": e_entry,
        "e_phoff": e_phoff,
        "e_shoff": e_shoff,
        "e_flags": e_flags,
        "e_ehsize": e_ehsize,
        "e_phentsize": e_phentsize,
        "e_phnum": e_phnum,
        "e_shentsize": e_shentsize,
        "e_shnum": e_shnum,
        "e_shstrndx": e_shstrndx,
    }
    phdrs: list[dict[str, int]] = []
    for i in range(e_phnum):
        off = e_phoff + i * 32
        if off + 32 > len(elf_bytes):
            raise ValueError(f"PHDR[{i}] at 0x{off:x} exceeds ELF length 0x{len(elf_bytes):x}")
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
            "<IIIIIIII", elf_bytes, off
        )
        phdrs.append(
            {
                "idx": i,
                "p_type": p_type,
                "p_offset": p_offset,
                "p_vaddr": p_vaddr,
                "p_paddr": p_paddr,
                "p_filesz": p_filesz,
                "p_memsz": p_memsz,
                "p_flags": p_flags,
                "p_align": p_align,
            }
        )
    return hdr, phdrs


def _load_payload(payload_dir: Path) -> dict[str, Any]:
    """Load and cryptographically verify `coexist_nikondma` binaries + `coexist_manifest.json`."""
    manifest_path = payload_dir / "coexist_manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Missing coexist_manifest.json in {payload_dir}")
    man = json.loads(manifest_path.read_text(encoding="utf-8"))

    secs = man["sections"]
    audits = man.get("audits", {})
    if not audits.get("all_passed", False):
        raise ValueError("Refusing to patch: coexist_manifest audits.all_passed is not True")
    if secs.get(".data", {}).get("size_bytes", 0) != 0:
        raise ValueError("FATAL: non-zero .data in XM6 payload")
    if secs.get(".bss", {}).get("size_bytes", 0) != 0:
        raise ValueError("FATAL: non-zero .bss in XM6 payload")
    if audits.get("linked_unresolved_symbols"):
        raise ValueError(f"FATAL: unresolved symbols in XM6 payload: {audits['linked_unresolved_symbols']}")
    if audits.get("fp_instructions_found", 0) != 0 or audits.get("soft_fp_symbols_found", 0) != 0:
        raise ValueError("FATAL: FP instructions or soft-FP symbols in XM6 payload")
    if audits.get("hdf_v1_dead_symbols_found", 0) != 0:
        raise ValueError("FATAL: old HDF v1 dead symbols present in XM6 payload")
    rcheck = audits.get("rcheck_full", {})
    if not rcheck or not all(v.get("passed") for v in rcheck.values()):
        raise ValueError("FATAL: ceva-elf-rcheck -full did not pass for all objects/linked ELF")

    text_sec = secs[".text.z8fx"]
    ro_sec = secs[".rodata.z8fx"]

    text_vma = int(text_sec["vma"], 16)
    text_hw_path = payload_dir / text_sec["expeed7_reversed_file"]
    text_hw = text_hw_path.read_bytes()
    if len(text_hw) != int(text_sec["size_bytes"]):
        raise ValueError(f".text.z8fx size mismatch: file={len(text_hw)} vs manifest={text_sec['size_bytes']}")
    if _sha(text_hw) != text_sec["expeed7_reversed_sha256"]:
        raise ValueError(f".text.z8fx SHA-256 mismatch for {text_hw_path}")
    if text_vma != EXT_TEXT_MIN or len(text_hw) > EXT_TEXT_MAX_BUDGET or (len(text_hw) % 32) != 0:
        raise ValueError(f".text.z8fx violates Z8 DRAM code budget: vma=0x{text_vma:08x}, size={len(text_hw)}")

    # Cross-verify 16-byte chunk reversal against raw LE copy when present
    raw_le_name = text_sec.get("raw_le_file")
    if raw_le_name and (payload_dir / raw_le_name).is_file():
        raw_le = (payload_dir / raw_le_name).read_bytes()
        if _sha(raw_le) != text_sec["raw_le_sha256"]:
            raise ValueError(f".text.z8fx raw LE SHA-256 mismatch for {raw_le_name}")
        if _bswap16_chunks(raw_le) != text_hw:
            raise ValueError(f"FATAL: {text_hw_path.name} is NOT exact 16B reversal of {raw_le_name}")

    ro_vma = int(ro_sec["vma"], 16)
    ro_path = payload_dir / ro_sec["file"]
    ro_bytes = ro_path.read_bytes()
    if len(ro_bytes) != int(ro_sec["size_bytes"]):
        raise ValueError(f".rodata.z8fx size mismatch: file={len(ro_bytes)} vs manifest={ro_sec['size_bytes']}")
    if _sha(ro_bytes) != ro_sec["sha256"]:
        raise ValueError(f".rodata.z8fx SHA-256 mismatch for {ro_path}")
    if ro_sec.get("byte_order") != "little_endian":
        raise ValueError(f"Unexpected .rodata.z8fx byte_order: {ro_sec.get('byte_order')}")
    if ro_vma < IDM_RODATA_MIN or len(ro_bytes) > IDM_RODATA_MAX_BUDGET or (ro_vma + len(ro_bytes)) > IDM_STACK_START:
        raise ValueError(f".rodata.z8fx violates IDM rodata budget: vma=0x{ro_vma:08x}, size={len(ro_bytes)}")

    slot_patches: dict[int, int] = {}
    for p in man["function_table_patch"]["patches"]:
        slot = int(p["slot"])
        vma = int(p["new_vma"], 16)
        if slot not in (39, 40, 41):
            raise ValueError(f"Refusing to patch non-reserved OEM slot {slot}")
        if (vma % 32) != 0 or not (text_vma <= vma < text_vma + len(text_hw)):
            raise ValueError(f"Slot {slot} target 0x{vma:08x} misaligned or outside .text.z8fx")
        slot_patches[slot] = vma
    if set(slot_patches.keys()) != {39, 40, 41}:
        raise ValueError(f"Expected exactly slots {{39, 40, 41}}, got {sorted(slot_patches.keys())}")

    return {
        "manifest": man,
        "variant": man.get("variant", payload_dir.name),
        "text_vma": text_vma,
        "text_bytes": text_hw,
        "text_sha256": _sha(text_hw),
        "text_raw_le_sha256": text_sec["raw_le_sha256"],
        "rodata_vma": ro_vma,
        "rodata_bytes": ro_bytes,
        "rodata_sha256": _sha(ro_bytes),
        "slot_patches": slot_patches,
    }


# ---------------------------------------------------------------------------
# Bit-Accurate Simulator for ARM64 Loader 0x40c92194 + SRAM Copy 0x40c92434
# ---------------------------------------------------------------------------
def simulate_arm64_loader_0x40c92194(
    elf_bytes: bytes | bytearray,
    *,
    expected_segments: list[dict[str, Any]] | None = None,
    expected_slots: dict[int, int] | None = None,
    stock_elf_bytes: bytes | None = None,
) -> dict[str, Any]:
    """Execute the exact instruction-level logic of Z8 C3.11 ARM64 loader 0x40c92194
    and hardware SRAM copy 0x40c92434(core=0) on an emulated 16 MiB DSP DRAM pool
    (0x0a01800000..0x0a02800000), proving:
      - loader_fault_flag == 0
      - overlaps == 0 (excluding OEM stock PHDR[7]/PHDR[8] zero-fill overlap)
      - stock_oem_regions_preserved == True (verified cryptographically against stock
        PMSS, DRAM .text, .data.external, all 61 untouched function table slots,
        all pre-rodata IDM, and IDM stack 0x3d000..0x40000!)
      - exact placement of PHDR[14] (.text.z8fx) & PHDR[15] (.rodata.z8fx) & slots 39..41.
    """
    elf_b = bytes(elf_bytes)
    if len(elf_b) != ELF0_EXACT_SIZE:
        raise AssertionError(f"ELF0 length {len(elf_b)} != {ELF0_EXACT_SIZE}")
    if elf_b[1:4] != b"ELF":
        raise AssertionError("Loader 0x40c921bc magic check failed (elf[1..3] != 'ELF')")

    e_phoff = struct.unpack_from("<I", elf_b, 0x1C)[0]
    e_phnum = struct.unpack_from("<H", elf_b, 0x2C)[0]
    if e_phoff == 0 or e_phnum == 0:
        raise AssertionError(f"Loader 0x40c921e0/e8 failed: e_phoff=0x{e_phoff:x}, e_phnum={e_phnum}")

    pool = bytearray(b"\xcc" * (DSP_POOL_END - DSP_POOL_BASE))
    fault_flag = 0
    loaded_spans: list[tuple[int, int, str, int]] = []

    for i in range(e_phnum):
        ph_off = e_phoff + i * 0x20
        if ph_off + 0x20 > len(elf_b):
            raise AssertionError(f"PHDR[{i}] at 0x{ph_off:x} reads past ELF0 end 0x{len(elf_b):x}")
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, p_flags, _p_align = struct.unpack_from(
            "<IIIIIIII", elf_b, ph_off
        )
        if p_type != PT_LOAD:
            continue

        top2 = (p_vaddr >> 30) & 0x3
        if top2 == 0:
            if (p_flags & PF_X) != 0:
                dst_va = CORE0_PMSS_STAGE + p_vaddr
                region = "PMSS"
                if p_vaddr + p_memsz > PMSS_SIZE:
                    raise AssertionError(f"PHDR[{i}] PMSS overflow: 0x{p_vaddr:x}+0x{p_memsz:x}")
            else:
                dst_va = CORE0_IDM_STAGE + p_vaddr
                region = "IDM"
                if p_vaddr + p_memsz > IDM_SIZE:
                    raise AssertionError(f"PHDR[{i}] IDM overflow: 0x{p_vaddr:x}+0x{p_memsz:x}")
        else:
            rel = (p_vaddr - IOMMU_WIN60_DSP_BASE) & 0xFFFFFFFF
            if p_vaddr < IOMMU_WIN60_DSP_BASE or rel >= IOMMU_WIN60_SIZE:
                raise AssertionError(f"PHDR[{i}] IOMMU win 0x60 failed for p_vaddr=0x{p_vaddr:08x}")
            dst_va = IOMMU_WIN60_ARM_BASE + rel
            region = "DRAM"

        if p_filesz != 0:
            if p_offset + p_filesz > len(elf_b):
                raise AssertionError(f"PHDR[{i}] source OOB: off=0x{p_offset:x}+fsz=0x{p_filesz:x}")
            if dst_va < DSP_POOL_BASE or (dst_va + p_filesz) >= DSP_POOL_END:
                fault_flag = 1
            else:
                poff = dst_va - DSP_POOL_BASE
                pool[poff : poff + p_filesz] = elf_b[p_offset : p_offset + p_filesz]
                loaded_spans.append((dst_va, dst_va + p_filesz, f"{region}_COPY", i))

        if p_memsz < p_filesz:
            raise AssertionError(f"PHDR[{i}] p_memsz (0x{p_memsz:x}) < p_filesz (0x{p_filesz:x})")
        bss_len = p_memsz - p_filesz
        if bss_len != 0:
            bss_va = dst_va + p_filesz
            if bss_va < DSP_POOL_BASE or (bss_va + bss_len) >= DSP_POOL_END:
                fault_flag = 1
            else:
                poff = bss_va - DSP_POOL_BASE
                pool[poff : poff + bss_len] = b"\x00" * bss_len
                loaded_spans.append((bss_va, bss_va + bss_len, f"{region}_ZERO", i))

    if fault_flag != 0:
        raise AssertionError("Loader 0x40c92194 set FAULT FLAG *(u8*)0x0a02e803ee = 1")

    # Zero unintended overlaps (allow ONLY OEM stock PHDR[7]/PHDR[8] IDM_ZERO overlap)
    overlaps = []
    for a in range(len(loaded_spans)):
        s0, e0, k0, i0 = loaded_spans[a]
        for b in range(a + 1, len(loaded_spans)):
            s1, e1, k1, i1 = loaded_spans[b]
            if max(s0, s1) < min(e0, e1):
                if {i0, i1} == {7, 8} and k0 == "IDM_ZERO" and k1 == "IDM_ZERO":
                    continue
                overlaps.append((i0, k0, hex(s0), hex(e0), i1, k1, hex(s1), hex(e1)))
    if overlaps:
        raise AssertionError(f"Memory overlap detected during 0x40c92194 load: {overlaps}")

    # 0x40c92434(core=0): Hardware SRAM copy
    hw_pmss = bytes(pool[0x000000 : 0x000000 + PMSS_SIZE])
    hw_idm = bytes(pool[0x010000 : 0x010000 + IDM_SIZE])

    # Cryptographic verification of 100% stock OEM loaded region preservation
    if _sha(hw_pmss) != STOCK_PMSS_SHA256:
        raise AssertionError("Stock PMSS SRAM (0x1c200000..0x1c210000) was altered!")
    if _sha(pool[0x50000 : 0x50000 + 0x74580]) != STOCK_DRAM_TEXT_SHA256:
        raise AssertionError("Stock DRAM .text (0x41850000..0x418c4580) was altered!")
    if _sha(pool[0x1B0000 : 0x1B0004]) != STOCK_DATA_EXT_SHA256:
        raise AssertionError("Stock .data.external (0x419b0000..0x419b0004) was altered!")
    idm_pre_ro = bytearray(hw_idm[:IDM_RODATA_MIN])
    idm_pre_ro[SLOT_39_IDM_VMA : SLOT_39_IDM_VMA + 12] = b"\x00" * 12
    if _sha(idm_pre_ro) != STOCK_IDM_PRE_RO_MASK_SHA256:
        raise AssertionError("Untouched OEM IDM (<0x3bc40 or OEM slots 0..38/42..63) was altered!")
    if _sha(hw_idm[IDM_STACK_START:]) != STOCK_IDM_STACK_SHA256:
        raise AssertionError("IDM Stack region (0x0003d000..0x00040000) was altered!")

    # Optional direct byte comparison against stock_elf_bytes when provided
    if stock_elf_bytes is not None:
        stock_sim = simulate_arm64_loader_0x40c92194(stock_elf_bytes)
        if hw_pmss != stock_sim["_pmss"]:
            raise AssertionError("Direct stock comparison: PMSS mismatch!")
        if bytes(pool[0x50000 : 0x50000 + 0x74580]) != stock_sim["_pool"][0x50000 : 0x50000 + 0x74580]:
            raise AssertionError("Direct stock comparison: DRAM .text mismatch!")
        if bytes(pool[0x1B0000 : 0x1B0004]) != stock_sim["_pool"][0x1B0000 : 0x1B0004]:
            raise AssertionError("Direct stock comparison: .data.external mismatch!")
        for slot in range(FUNC_TABLE_SLOTS):
            if slot in (39, 40, 41):
                continue
            off = FUNC_TABLE_IDM_VMA + slot * 4
            if hw_idm[off : off + 4] != stock_sim["_idm"][off : off + 4]:
                raise AssertionError(f"Direct stock comparison: OEM slot {slot} altered!")
        if hw_idm[IDM_STACK_START:] != stock_sim["_idm"][IDM_STACK_START:]:
            raise AssertionError("Direct stock comparison: IDM stack altered!")

    # Verify expected new overlay segments in simulated hardware memory
    verified_segments: list[str] = []
    if expected_segments is not None:
        for seg in expected_segments:
            vma = seg["vma_int"]
            size = seg["size_bytes"]
            want_sha = seg["sha256"]
            if vma >= IOMMU_WIN60_DSP_BASE:
                arm_va = IOMMU_WIN60_ARM_BASE + (vma - IOMMU_WIN60_DSP_BASE)
                poff = arm_va - DSP_POOL_BASE
                got = bytes(pool[poff : poff + size])
                if _sha(got) != want_sha:
                    raise AssertionError(f"Simulated DRAM SHA mismatch for {seg['name']} @ 0x{vma:08x}")
                verified_segments.append(
                    f"{seg['name']}: DSP 0x{vma:08x}..0x{vma+size:08x} -> ARM 0x{arm_va:010x} ({size} B EXACT)"
                )
            else:
                got = hw_idm[vma : vma + size]
                if _sha(got) != want_sha:
                    raise AssertionError(f"Simulated HW IDM SHA mismatch for {seg['name']} @ 0x{vma:08x}")
                verified_segments.append(
                    f"{seg['name']}: IDM 0x{vma:08x}..0x{vma+size:08x} -> HW 0x{0x1c000000+vma:08x} ({size} B EXACT)"
                )

    if expected_slots is not None:
        for slot, want_vma in expected_slots.items():
            idm_off = FUNC_TABLE_IDM_VMA + slot * 4
            got_vma = struct.unpack_from("<I", hw_idm, idm_off)[0]
            if got_vma != want_vma:
                raise AssertionError(
                    f"HW IDM Slot {slot} @ 0x{0x1c000000+idm_off:08x}: got 0x{got_vma:08x}, want 0x{want_vma:08x}"
                )

    return {
        "loader_fault_flag": fault_flag,
        "phdrs_processed": e_phnum,
        "loaded_spans": len(loaded_spans),
        "overlaps": 0,
        "stock_oem_regions_preserved": True,
        "verified_segments": verified_segments,
        "hw_slot_39": f"0x{struct.unpack_from('<I', hw_idm, SLOT_39_IDM_VMA)[0]:08x}",
        "hw_slot_40": f"0x{struct.unpack_from('<I', hw_idm, SLOT_40_IDM_VMA)[0]:08x}",
        "hw_slot_41": f"0x{struct.unpack_from('<I', hw_idm, SLOT_41_IDM_VMA)[0]:08x}",
        "_pmss": hw_pmss,
        "_idm": hw_idm,
        "_pool": bytes(pool),
    }


def _strip_sim_private(sim: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in sim.items() if not k.startswith("_")}


def _build_changed_ranges(
    patched_elf0: bytes,
    text_off: int,
    text_end: int,
    ro_off: int,
    ro_end: int,
    phdr_off: int,
    phdr_end: int,
) -> list[dict[str, Any]]:
    """Build exact main.main file-offset & VA changed_ranges records for audit_whole_image.py."""
    raw_specs = [
        ("ehdr_phoff_shoff",          0x00001C, 0x000024),
        ("ehdr_phnum",                0x00002C, 0x00002E),
        ("ehdr_shnum_shstrndx",       0x000030, 0x000034),
        ("legacy_phdr_table_zeroed",  LEGACY_PHDR_OFFSET, LEGACY_PHDR_OFFSET + LEGACY_PHDR_BYTES),
        ("function_table_slots_39_41", SLOT_39_ELF0_OFF, SLOT_41_ELF0_END),
        ("phdr14_text_z8fx",          text_off, text_end),
        ("phdr15_rodata_z8fx",        ro_off,   ro_end),
        ("elf0_tail_free_zeroed",     ro_end,   phdr_off),
        ("relocated_phdr_table",      phdr_off, phdr_end),
        ("elf0_tail_pad_zeroed",      phdr_end, ELF0_EXACT_SIZE),
    ]
    out: list[dict[str, Any]] = []
    for name, e0, e1 in raw_specs:
        if e1 <= e0:
            continue
        f0 = ELF0_MAIN_OFFSET + e0
        f1 = ELF0_MAIN_OFFSET + e1
        out.append(
            {
                "name": name,
                "file_off": f0,
                "end_off": f1,
                "file_off_hex": f"0x{f0:08x}",
                "end_off_hex": f"0x{f1:08x}",
                "size_bytes": f1 - f0,
                "va": f"0x{BASE + f0:08x}",
                "end_va": f"0x{BASE + f1:08x}",
                "elf0_off": f"0x{e0:06x}",
                "elf0_end_off": f"0x{e1:06x}",
                "sha256": _sha(patched_elf0[e0:e1]),
            }
        )
    return out


def _compose_patched_elf0(
    base_elf0: bytes,
    payload: dict[str, Any],
    *,
    is_stock_input: bool,
) -> tuple[bytes, dict[str, Any]]:
    """Deterministically compose the patched 692,516 B ELF0 from either stock or already-patched ELF0."""
    hdr, orig_phdrs = _parse_elf32(base_elf0)
    if len(orig_phdrs) < STOCK_PHNUM:
        raise ValueError(f"Expected >= {STOCK_PHNUM} PHDRs, got {len(orig_phdrs)}")
    base_phdrs = orig_phdrs[:STOCK_PHNUM]

    # Verify the 14 OEM base PHDRs match Z8 C3.11 stock bit-for-bit
    packed_base_phdrs = b"".join(
        struct.pack(
            "<IIIIIIII",
            ph["p_type"], ph["p_offset"], ph["p_vaddr"], ph["p_paddr"],
            ph["p_filesz"], ph["p_memsz"], ph["p_flags"], ph["p_align"],
        )
        for ph in base_phdrs
    )
    if _sha(packed_base_phdrs) != STOCK_BASE_PHDRS_SHA256:
        raise ValueError(f"Base OEM PHDR[0..13] mismatch: {_sha(packed_base_phdrs)} != {STOCK_BASE_PHDRS_SHA256}")

    if is_stock_input:
        if hdr["e_phoff"] != LEGACY_PHDR_OFFSET or hdr["e_phnum"] != STOCK_PHNUM:
            raise ValueError(f"Unexpected stock ELF0 header: e_phoff=0x{hdr['e_phoff']:x}, e_phnum={hdr['e_phnum']}")

    out = bytearray(base_elf0)

    # 1. Patch g_NikonDspFrameworkFunctionTable[39..41]
    slot_patches = payload["slot_patches"]
    slot_report = []
    for slot in (39, 40, 41):
        new_vma = slot_patches[slot]
        idm_vma = FUNC_TABLE_IDM_VMA + slot * 4
        elf0_off = IDM_DATA_FILE_OFF + (idm_vma - IDM_DATA_VMA)
        old_val = struct.unpack_from("<I", out, elf0_off)[0]
        if is_stock_input and old_val != NOP_FUNCTION_VMA:
            raise ValueError(f"Slot {slot} @ elf0+0x{elf0_off:06x}: expected stock nop 0x{NOP_FUNCTION_VMA:08x}, got 0x{old_val:08x}")
        struct.pack_into("<I", out, elf0_off, new_vma)
        slot_report.append(
            {
                "slot": slot,
                "idm_vma": f"0x{idm_vma:08x}",
                "elf0_offset": f"0x{elf0_off:06x}",
                "main_main_offset": f"0x{ELF0_MAIN_OFFSET + elf0_off:08x}",
                "main_main_va": f"0x{BASE + ELF0_MAIN_OFFSET + elf0_off:08x}",
                "stock_vma": f"0x{NOP_FUNCTION_VMA:08x}",
                "new_vma": f"0x{new_vma:08x}",
            }
        )

    # 2. Zero non-loadable tail [0x0820a0 .. 0x0a9124]
    out[ELF_TAIL_PAYLOAD_START:ELF0_EXACT_SIZE] = b"\x00" * (ELF0_EXACT_SIZE - ELF_TAIL_PAYLOAD_START)

    # 3. Relocated PHDR table placement at tail (16 * 32 = 512 B @ 0x0a8f20..0x0a9120)
    phdr_table_bytes = EXPECTED_PATCHED_PHNUM * 32
    new_e_phoff = _align_down(ELF0_EXACT_SIZE - phdr_table_bytes, 16)
    phdr_table_end = new_e_phoff + phdr_table_bytes

    # 4. Pack PHDR[14] (.text.z8fx) and PHDR[15] (.rodata.z8fx)
    text_off = _align_up(ELF_TAIL_PAYLOAD_START, 32)
    text_end = text_off + len(payload["text_bytes"])
    ro_off = _align_up(text_end, 32)
    ro_end = ro_off + len(payload["rodata_bytes"])
    if ro_end > new_e_phoff:
        raise ValueError(
            f"Overlay payloads overflow ELF0 tail: end=0x{ro_end:06x} > e_phoff=0x{new_e_phoff:06x}"
        )

    out[text_off:text_end] = payload["text_bytes"]
    out[ro_off:ro_end] = payload["rodata_bytes"]

    new_phdrs = list(base_phdrs) + [
        {
            "idx": 14,
            "p_type": PT_LOAD,
            "p_offset": text_off,
            "p_vaddr": payload["text_vma"],
            "p_paddr": payload["text_vma"],
            "p_filesz": len(payload["text_bytes"]),
            "p_memsz": len(payload["text_bytes"]),
            "p_flags": PF_R | PF_X,
            "p_align": 32,
        },
        {
            "idx": 15,
            "p_type": PT_LOAD,
            "p_offset": ro_off,
            "p_vaddr": payload["rodata_vma"],
            "p_paddr": payload["rodata_vma"],
            "p_filesz": len(payload["rodata_bytes"]),
            "p_memsz": len(payload["rodata_bytes"]),
            "p_flags": PF_R,
            "p_align": 32,
        },
    ]

    # 5. Clear legacy PHDR slot (0x34..0x1f4) and write relocated 16-entry PHDR table
    out[LEGACY_PHDR_OFFSET : LEGACY_PHDR_OFFSET + LEGACY_PHDR_BYTES] = b"\x00" * LEGACY_PHDR_BYTES
    for i, ph in enumerate(new_phdrs):
        struct.pack_into(
            "<IIIIIIII",
            out,
            new_e_phoff + i * 32,
            ph["p_type"], ph["p_offset"], ph["p_vaddr"], ph["p_paddr"],
            ph["p_filesz"], ph["p_memsz"], ph["p_flags"], ph["p_align"],
        )

    # 6. Update Elf32_Ehdr (stripped section headers, relocated e_phoff, e_phnum=16)
    struct.pack_into("<I", out, 0x1C, new_e_phoff)             # e_phoff
    struct.pack_into("<I", out, 0x20, 0)                       # e_shoff = 0
    struct.pack_into("<H", out, 0x2C, EXPECTED_PATCHED_PHNUM)  # e_phnum = 16
    struct.pack_into("<H", out, 0x30, 0)                       # e_shnum = 0
    struct.pack_into("<H", out, 0x32, 0)                       # e_shstrndx = 0

    patched = bytes(out)
    assert len(patched) == ELF0_EXACT_SIZE

    added_segments = [
        {
            "phdr_idx": 14,
            "name": ".text.z8fx",
            "elf0_offset": f"0x{text_off:06x}",
            "elf0_end_offset": f"0x{text_end:06x}",
            "main_main_offset": f"0x{ELF0_MAIN_OFFSET + text_off:08x}",
            "main_main_end_offset": f"0x{ELF0_MAIN_OFFSET + text_end:08x}",
            "vma": f"0x{payload['text_vma']:08x}",
            "vma_int": payload["text_vma"],
            "end_vma": f"0x{payload['text_vma'] + len(payload['text_bytes']):08x}",
            "size_bytes": len(payload["text_bytes"]),
            "p_flags": f"0x{PF_R | PF_X:x}",
            "region": "EXTERNAL_DRAM",
            "byte_order": "expeed7_16byte_reversed",
            "sha256": payload["text_sha256"],
            "raw_le_sha256": payload["text_raw_le_sha256"],
        },
        {
            "phdr_idx": 15,
            "name": ".rodata.z8fx",
            "elf0_offset": f"0x{ro_off:06x}",
            "elf0_end_offset": f"0x{ro_end:06x}",
            "main_main_offset": f"0x{ELF0_MAIN_OFFSET + ro_off:08x}",
            "main_main_end_offset": f"0x{ELF0_MAIN_OFFSET + ro_end:08x}",
            "vma": f"0x{payload['rodata_vma']:08x}",
            "vma_int": payload["rodata_vma"],
            "end_vma": f"0x{payload['rodata_vma'] + len(payload['rodata_bytes']):08x}",
            "size_bytes": len(payload["rodata_bytes"]),
            "p_flags": f"0x{PF_R:x}",
            "region": "IDM_SRAM",
            "byte_order": "little_endian",
            "sha256": payload["rodata_sha256"],
        },
    ]

    changed_ranges = _build_changed_ranges(
        patched, text_off, text_end, ro_off, ro_end, new_e_phoff, phdr_table_end
    )
    merged_file_pairs = _merge_intervals([(r["file_off"], r["end_off"]) for r in changed_ranges])

    layout = {
        "e_phoff": f"0x{new_e_phoff:06x}",
        "e_phnum": EXPECTED_PATCHED_PHNUM,
        "phdr_table_bytes": phdr_table_bytes,
        "tail_payload_used_bytes": ro_end - ELF_TAIL_PAYLOAD_START,
        "tail_free_headroom_bytes": new_e_phoff - ro_end,
        "added_segments": added_segments,
        "slot_patches": slot_report,
        "changed_ranges": changed_ranges,
        "changed_file_offset_pairs": [[s, e] for s, e in merged_file_pairs],
        "changed_va_pairs": [[BASE + s, BASE + e] for s, e in merged_file_pairs],
        "changed_span_total_bytes": sum(e - s for s, e in merged_file_pairs),
    }
    return patched, layout


def verify(image: bytearray | bytes, meta: dict[str, Any]) -> dict[str, Any]:
    """Independently verify patched XM6 ELF0 inside `main.main` against `meta`:
      - Exact image size & ELF0/ELF1/ELF2 boundaries + 12B zero gap before ELF1
      - ELF1 & ELF2 100% unchanged vs stock C3.11 SHA-256
      - Patched ELF0 SHA-256, header fields, 16 PHDRs (14 stock OEM + PHDR[14..15]),
        zeroed legacy PHDR slot, zeroed tail headroom, Slot 39/40/41 words,
        and every `changed_ranges` sub-slice SHA-256
      - Bit-accurate `0x40c92194` ARM64 loader + `0x40c92434` SRAM copy simulation
        asserting `loader_fault_flag == 0`, `overlaps == 0`, `stock_oem_regions_preserved == True`.
    """
    if not isinstance(image, (bytes, bytearray)):
        raise TypeError("image must be bytes or bytearray")
    if len(image) != IMAGE_SIZE:
        raise ValueError(f"Unexpected main.main size: {len(image)} != {IMAGE_SIZE}")

    # 1. Gap + ELF1 + ELF2 integrity
    gap = bytes(image[ELF0_GAP_OFFSET : ELF0_GAP_OFFSET + ELF0_GAP_SIZE])
    if any(gap):
        raise ValueError(f"12B zero gap 0x{ELF0_GAP_OFFSET:08x}..0x{ELF1_MAIN_OFFSET:08x} is non-zero: {gap.hex()}")
    elf1 = bytes(image[ELF1_MAIN_OFFSET : ELF1_MAIN_OFFSET + ELF1_EXACT_SIZE])
    elf2 = bytes(image[ELF2_MAIN_OFFSET : ELF2_MAIN_OFFSET + ELF2_EXACT_SIZE])
    if elf1[:4] != b"\x7fELF" or _sha(elf1) != ELF1_STOCK_SHA256:
        raise ValueError(f"ELF1 @ 0x{ELF1_MAIN_OFFSET:08x} altered or invalid! sha={_sha(elf1)}")
    if elf2[:4] != b"\x7fELF" or _sha(elf2) != ELF2_STOCK_SHA256:
        raise ValueError(f"ELF2 @ 0x{ELF2_MAIN_OFFSET:08x} altered or invalid! sha={_sha(elf2)}")

    # 2. ELF0 header, PHDRs, and changed_ranges verification
    elf0 = bytes(image[ELF0_MAIN_OFFSET : ELF0_MAIN_END])
    if len(elf0) != ELF0_EXACT_SIZE:
        raise ValueError(f"ELF0 slice size {len(elf0)} != {ELF0_EXACT_SIZE}")

    if "elf0_patched_sha256" in meta and _sha(elf0) != meta["elf0_patched_sha256"]:
        raise ValueError(f"Patched ELF0 SHA-256 mismatch: {_sha(elf0)} != {meta['elf0_patched_sha256']}")

    hdr, phdrs = _parse_elf32(elf0)
    expected_phoff = int(meta["e_phoff"], 16)
    expected_phnum = int(meta["e_phnum"])
    if hdr["e_phoff"] != expected_phoff or hdr["e_phnum"] != expected_phnum:
        raise ValueError(f"ELF0 header mismatch: e_phoff=0x{hdr['e_phoff']:x} e_phnum={hdr['e_phnum']}")
    if hdr["e_shoff"] != 0 or hdr["e_shnum"] != 0 or hdr["e_shstrndx"] != 0:
        raise ValueError(f"ELF0 dangling section header fields: {hdr}")
    if any(elf0[LEGACY_PHDR_OFFSET : LEGACY_PHDR_OFFSET + LEGACY_PHDR_BYTES]):
        raise ValueError("Legacy PHDR slot 0x34..0x1f4 is not zeroed")

    # Verify base OEM PHDR[0..13] intact
    base_packed = b"".join(
        struct.pack(
            "<IIIIIIII",
            ph["p_type"], ph["p_offset"], ph["p_vaddr"], ph["p_paddr"],
            ph["p_filesz"], ph["p_memsz"], ph["p_flags"], ph["p_align"],
        )
        for ph in phdrs[:STOCK_PHNUM]
    )
    if _sha(base_packed) != STOCK_BASE_PHDRS_SHA256:
        raise ValueError("OEM base PHDR[0..13] corrupted in patched ELF0")

    # Verify every added segment against PHDR table & raw bytes
    expected_segments = []
    for seg in meta["added_segments"]:
        idx = int(seg["phdr_idx"])
        ph = phdrs[idx]
        vma = int(seg["vma"], 16)
        off = int(seg["elf0_offset"], 16)
        sz = int(seg["size_bytes"])
        fl = int(seg["p_flags"], 16)
        if (ph["p_type"], ph["p_offset"], ph["p_vaddr"], ph["p_filesz"], ph["p_memsz"], ph["p_flags"]) != (
            PT_LOAD, off, vma, sz, sz, fl
        ):
            raise ValueError(f"PHDR[{idx}] ({seg['name']}) descriptor mismatch: {ph}")
        if _sha(elf0[off : off + sz]) != seg["sha256"]:
            raise ValueError(f"PHDR[{idx}] ({seg['name']}) bytes SHA-256 mismatch")
        expected_segments.append({"name": seg["name"], "vma_int": vma, "size_bytes": sz, "sha256": seg["sha256"]})

    # Verify Slot 39/40/41 words in main.main
    expected_slots: dict[int, int] = {}
    for sp in meta["slot_patches"]:
        slot = int(sp["slot"])
        main_off = int(sp["main_main_offset"], 16)
        want_vma = int(sp["new_vma"], 16)
        got_vma = struct.unpack_from("<I", image, main_off)[0]
        if got_vma != want_vma:
            raise ValueError(f"Slot {slot} @ main+0x{main_off:08x}: got 0x{got_vma:08x}, want 0x{want_vma:08x}")
        expected_slots[slot] = want_vma

    # Verify every changed_ranges entry stays inside ELF0 and matches recorded SHA-256
    for rec in meta["changed_ranges"]:
        f0, f1 = int(rec["file_off"]), int(rec["end_off"])
        if not (ELF0_MAIN_OFFSET <= f0 < f1 <= ELF0_MAIN_END):
            raise ValueError(f"changed_ranges entry {rec['name']} escapes ELF0 bounds: 0x{f0:x}..0x{f1:x}")
        if _sha(image[f0:f1]) != rec["sha256"]:
            raise ValueError(f"changed_ranges entry {rec['name']} SHA-256 mismatch")

    # 3. Bit-accurate ARM64 loader 0x40c92194 + hardware SRAM copy 0x40c92434 simulation
    sim = simulate_arm64_loader_0x40c92194(
        elf0,
        expected_segments=expected_segments,
        expected_slots=expected_slots,
    )
    assert sim["loader_fault_flag"] == 0
    assert sim["overlaps"] == 0
    assert sim["stock_oem_regions_preserved"] is True

    return {
        "status": "PASS",
        "elf0_size_bytes": len(elf0),
        "elf0_patched_sha256": _sha(elf0),
        "gap_12b_zero": True,
        "elf1_unchanged": True,
        "elf1_sha256": ELF1_STOCK_SHA256,
        "elf2_unchanged": True,
        "elf2_sha256": ELF2_STOCK_SHA256,
        "changed_ranges_count": len(meta["changed_ranges"]),
        "loader_0x40c92194_sim": _strip_sim_private(sim),
    }


def apply(image: bytearray, payload_dir: Path | str | None = None) -> dict[str, Any]:
    """Atomically inject CEVA-XM6 `coexist_nikondma` into `z8_xm6_elf0.elf` inside `image`
    (in-place, zero shift, exact 692,516 B size, idempotent if already patched with the
    same payload).

    Does NOT restamp Bootguard Build ID at 0x401fe060; Stage 0 restamps Build ID once
    after all Stage 0 patches complete.
    """
    if not isinstance(image, bytearray):
        raise TypeError("image must be a mutable bytearray")
    if len(image) != IMAGE_SIZE:
        raise ValueError(f"Unexpected main.main size: {len(image)} != {IMAGE_SIZE}")

    pdir = Path(payload_dir) if payload_dir is not None else DEFAULT_PAYLOAD_DIR
    payload = _load_payload(pdir)

    # Pre-check 12B zero gap + ELF1 + ELF2 integrity BEFORE touching any byte
    gap_before = bytes(image[ELF0_GAP_OFFSET : ELF0_GAP_OFFSET + ELF0_GAP_SIZE])
    if any(gap_before):
        raise ValueError(f"Pre-patch 12B zero gap before ELF1 is non-zero: {gap_before.hex()}")
    elf1_before = bytes(image[ELF1_MAIN_OFFSET : ELF1_MAIN_OFFSET + ELF1_EXACT_SIZE])
    elf2_before = bytes(image[ELF2_MAIN_OFFSET : ELF2_MAIN_OFFSET + ELF2_EXACT_SIZE])
    if elf1_before[:4] != b"\x7fELF" or _sha(elf1_before) != ELF1_STOCK_SHA256:
        raise ValueError(f"Pre-patch ELF1 @ 0x{ELF1_MAIN_OFFSET:08x} mismatch: sha={_sha(elf1_before)}")
    if elf2_before[:4] != b"\x7fELF" or _sha(elf2_before) != ELF2_STOCK_SHA256:
        raise ValueError(f"Pre-patch ELF2 @ 0x{ELF2_MAIN_OFFSET:08x} mismatch: sha={_sha(elf2_before)}")

    elf0_before = bytes(image[ELF0_MAIN_OFFSET : ELF0_MAIN_END])
    elf0_before_sha = _sha(elf0_before)

    if elf0_before_sha == ELF0_STOCK_SHA256:
        mode = "applied"
        patched_elf0, layout = _compose_patched_elf0(elf0_before, payload, is_stock_input=True)
        # Prove EVERY changed byte between stock ELF0 and patched ELF0 is accounted for in changed_ranges
        elf0_diff_runs = _diff_runs(elf0_before, patched_elf0)
        main_diff_runs = [(ELF0_MAIN_OFFSET + s, ELF0_MAIN_OFFSET + e) for s, e in elf0_diff_runs]
        covered = [(r["file_off"], r["end_off"]) for r in layout["changed_ranges"]]
        unexplained = _subtract_runs(main_diff_runs, covered)
        if unexplained:
            raise AssertionError(f"Unexplained byte diffs in ELF0 patch: {unexplained[:10]}")
        actual_changed_bytes = sum(e - s for s, e in elf0_diff_runs)
        sim_full = simulate_arm64_loader_0x40c92194(
            patched_elf0,
            expected_segments=layout["added_segments"],
            expected_slots=payload["slot_patches"],
            stock_elf_bytes=elf0_before,
        )
    else:
        # Idempotent check: verify existing ELF0 is ALREADY identically patched with this exact payload
        hdr, phdrs = _parse_elf32(elf0_before)
        if hdr["e_phnum"] != EXPECTED_PATCHED_PHNUM or hdr["e_shoff"] != 0 or hdr["e_shnum"] != 0:
            raise ValueError(
                f"Input ELF0 is neither stock ({ELF0_STOCK_SHA256}) nor a valid {EXPECTED_PATCHED_PHNUM}-PHDR "
                f"coexist_nikondma ELF0 (got sha={elf0_before_sha}, e_phnum={hdr['e_phnum']})"
            )
        recomposed, layout = _compose_patched_elf0(elf0_before, payload, is_stock_input=False)
        if recomposed != elf0_before:
            raise ValueError(
                f"Input ELF0 appears already patched with a DIFFERENT payload (sha={elf0_before_sha} != expected {_sha(recomposed)})"
            )
        mode = "already_patched"
        patched_elf0 = elf0_before
        actual_changed_bytes = 0
        sim_full = simulate_arm64_loader_0x40c92194(
            patched_elf0,
            expected_segments=layout["added_segments"],
            expected_slots=payload["slot_patches"],
        )

    assert sim_full["loader_fault_flag"] == 0
    assert sim_full["overlaps"] == 0
    assert sim_full["stock_oem_regions_preserved"] is True

    # Build public metadata (strip internal vma_int helper keys)
    pub_segments = [{k: v for k, v in seg.items() if k != "vma_int"} for seg in layout["added_segments"]]
    meta: dict[str, Any] = {
        "schema": "v71r6.patch_xm6_elf0_stage0/v1",
        "mode": mode,
        "variant": payload["variant"],
        "payload_dir": str(pdir),
        "elf0_main_offset": f"0x{ELF0_MAIN_OFFSET:08x}",
        "elf0_main_end_offset": f"0x{ELF0_MAIN_END:08x}",
        "elf0_main_va": f"0x{ELF0_MAIN_VA:08x}",
        "elf0_size_bytes": ELF0_EXACT_SIZE,
        "elf0_stock_sha256": ELF0_STOCK_SHA256,
        "elf0_input_sha256": elf0_before_sha,
        "elf0_patched_sha256": _sha(patched_elf0),
        "e_phoff": layout["e_phoff"],
        "e_phnum": layout["e_phnum"],
        "phdr_table_bytes": layout["phdr_table_bytes"],
        "tail_payload_used_bytes": layout["tail_payload_used_bytes"],
        "tail_free_headroom_bytes": layout["tail_free_headroom_bytes"],
        "actual_mutated_bytes_this_call": actual_changed_bytes,
        "changed_span_total_bytes": layout["changed_span_total_bytes"],
        "added_segments": pub_segments,
        "slot_patches": layout["slot_patches"],
        "changed_ranges": layout["changed_ranges"],
        "changed_file_offset_pairs": layout["changed_file_offset_pairs"],
        "changed_va_pairs": layout["changed_va_pairs"],
        "gap_12b_zero": True,
        "elf1_unchanged": True,
        "elf1_sha256": ELF1_STOCK_SHA256,
        "elf2_unchanged": True,
        "elf2_sha256": ELF2_STOCK_SHA256,
        "loader_0x40c92194_sim": _strip_sim_private(sim_full),
    }

    # Atomic commit onto scratch-verified slice
    if mode == "applied":
        image[ELF0_MAIN_OFFSET:ELF0_MAIN_END] = patched_elf0

    # Independent post-commit full verify() pass
    ver = verify(image, meta)
    assert ver["status"] == "PASS"
    meta["verify_passed"] = True
    return meta


def restamp_build_id(image: bytearray) -> int:
    off = BUILD_ID_ADDR - BASE
    struct.pack_into("<I", image, off, 0)
    build_id = struct.unpack("<I", hashlib.sha256(image).digest()[:4])[0] | 0x01000000
    struct.pack_into("<I", image, off, build_id)
    return build_id


def main() -> int:
    ap = argparse.ArgumentParser(
        description="V71.6-XM6 Stage 0 CEVA-XM6 Core 0 ELF0 in-place patcher & verifier"
    )
    ap.add_argument("--in", dest="inp", required=True, type=Path, help="Input main.main")
    ap.add_argument("--out", dest="out", type=Path, default=None, help="Output main.main (omit with --verify-only)")
    ap.add_argument("--payload-dir", type=Path, default=None, help="Override coexist_nikondma payload dir")
    ap.add_argument("--meta-in", type=Path, default=None, help="Meta JSON for --verify-only")
    ap.add_argument("--meta-out", type=Path, default=None, help="Optional output path for meta JSON")
    ap.add_argument("--verify-only", action="store_true", help="Only run verify() against --meta-in")
    ap.add_argument("--restamp-build-id", action="store_true", help="Restamp Bootguard Build ID at 0x401fe060")
    args = ap.parse_args()

    img = bytearray(args.inp.read_bytes())
    if args.verify_only:
        if args.meta_in is None:
            ap.error("--verify-only requires --meta-in")
        meta = json.loads(args.meta_in.read_text(encoding="utf-8"))
        res = verify(img, meta)
        print(json.dumps(res, indent=2))
        return 0

    meta = apply(img, payload_dir=args.payload_dir)
    if args.restamp_build_id:
        meta["build_id"] = hex(restamp_build_id(img))
    meta["out_main_sha256"] = _sha(img)
    if args.out is not None:
        args.out.write_bytes(img)
    if args.meta_out is not None:
        args.meta_out.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(meta, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
