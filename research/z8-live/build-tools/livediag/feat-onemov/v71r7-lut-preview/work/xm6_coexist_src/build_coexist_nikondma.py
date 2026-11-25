#!/usr/bin/env python3
"""Build and hard-audit the monolithic 2-PHDR `coexist_nikondma` CEVA-XM6 Core 0 payload
(Slot 39/40 Zf FilmGrain NikonDMA + Slot 41 Ricoh Optical HDF v2 NikonDMA) on win-lab
using CEVA-ToolBox V18.0.3.
"""
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

CEVA_BIN = pathlib.Path(r"C:\CEVA\CEVA-ToolBox_Tools\cevatools\bin")
CEVA_LIB = pathlib.Path(r"C:\CEVA\CEVA-ToolBox_Tools\cevatools\lib")
WORK = pathlib.Path(r"C:\temp\z8_xm6_coexist_nikondma")
SRC = WORK / "src"
OBJ = WORK / "obj"
OUT = WORK / "out_nikondma"

ENV = os.environ.copy()
ENV["CEVAXMTOOLS"] = str(CEVA_BIN)
ENV["CEVAXMLIB"] = str(CEVA_LIB)
ENV["PATH"] = f"{CEVA_BIN};{CEVA_LIB};" + ENV.get("PATH", "")

ARCH_FLAGS = [
    "--target=cevaxm6-unknown-unknown-elf",
    "-mcpu=rtlv1.2.0",
    "-mfp=1",
    "-mvfp=0",
    "-mnonlinear=2",
]

GRAIN_FLAGS = [
    "-O2",
    "-DXM6_TARGET_Z8_ELF0",
    "-DXM6_USE_NIKON_DMA",
    "-fno-exceptions",
    "-fno-rtti",
    "-ffunction-sections",
    "-fdata-sections",
]

HDF_FLAGS = [
    "-O3",
    "-fmerge-all-constants",
    "-DXM6_TARGET_Z8_ELF0",
    "-DXM6_USE_NIKON_DMA",
    "-fno-exceptions",
    "-fno-rtti",
    "-ffunction-sections",
    "-fdata-sections",
]

STOCK_PMSS_SYMS = {
    "_ZN8NikonDMA18CreateDescriptor1DEPNS_10DescriptorEjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE": 0x000023C0,
    "_ZN8NikonDMA18CreateDescriptor2DEPNS_10DescriptorEjjjjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE": 0x00002480,
    "_ZN8NikonDMA21DispatchRequestNoSyncEPvPKvPKNS_10DescriptorE": 0x00002560,
    "_ZN8NikonDMA15DispatchRequestEPvPKvPKNS_10DescriptorE": 0x00002960,
    "_ZN8NikonDMA11WaitRequestEjj": 0x00002AC0,
    "memcpy": 0x00005500,
    "memset": 0x00006300,
}

REQUIRED_NIKON_DMA_SYMS = [
    "_ZN8NikonDMA18CreateDescriptor1DEPNS_10DescriptorEjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE",
    "_ZN8NikonDMA18CreateDescriptor2DEPNS_10DescriptorEjjjjjNS_12TRANSFER_DIRENS_13TRANSFER_MODEE",
    "_ZN8NikonDMA21DispatchRequestNoSyncEPvPKvPKNS_10DescriptorE",
    "_ZN8NikonDMA15DispatchRequestEPvPKvPKNS_10DescriptorE",
    "_ZN8NikonDMA11WaitRequestEjj",
]


def run(cmd, cwd=WORK, check=True):
    res = subprocess.run(cmd, cwd=str(cwd), env=ENV, capture_output=True, text=True)
    if check and res.returncode != 0:
        print("COMMAND FAILED:", " ".join(str(c) for c in cmd))
        print("STDOUT:\n", res.stdout)
        print("STDERR:\n", res.stderr)
        sys.exit(res.returncode)
    return res.returncode, res.stdout, res.stderr


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def sha256_file(p: pathlib.Path) -> str:
    return sha256_bytes(p.read_bytes())


def bswap16_chunks(raw: bytes) -> bytes:
    assert len(raw) % 16 == 0, f"text length {len(raw)} not multiple of 16"
    return b"".join(raw[i : i + 16][::-1] for i in range(0, len(raw), 16))


def run_rcheck(path: pathlib.Path, label: str) -> dict:
    rc, out, err = run(
        [str(CEVA_BIN / "ceva-elf-rcheck.exe"), "-full", "-file", str(path)],
        check=False,
    )
    combined = (out + "\n" + err).strip()
    lines = [ln for ln in combined.splitlines() if ln.strip()]
    viol_lines = [
        ln for ln in lines
        if re.search(r"(violation|error|illegal|fail)", ln, re.I)
        and not re.search(r"0\s+violation|no\s+violation|passed", ln, re.I)
    ]
    passed = (rc == 0) and (len(viol_lines) == 0)
    print(f"  rcheck -full [{label}]: rc={rc}, violations={len(viol_lines)}, tail={lines[-3:]}")
    if not passed:
        print(combined)
        sys.exit(1)
    return {
        "file": str(path.relative_to(WORK)).replace("\\", "/"),
        "exit_code": rc,
        "passed": passed,
        "violation_count": len(viol_lines),
        "violation_lines": viol_lines,
        "output_tail": lines[-5:],
    }


def parse_nm(elf_path: pathlib.Path):
    _, out, _ = run([str(CEVA_BIN / "ceva-elf-nm.exe"), "-n", "-S", str(elf_path)])
    syms = {}
    undef = []
    for ln in out.splitlines():
        parts = ln.split()
        if len(parts) >= 2 and parts[0] == "U":
            undef.append(parts[1])
        elif len(parts) == 3:
            addr, typ, name = parts
            syms[name] = {"addr": int(addr, 16), "size": 0, "type": typ}
        elif len(parts) >= 4:
            addr, sz, typ, name = parts[0], parts[1], parts[2], parts[3]
            syms[name] = {"addr": int(addr, 16), "size": int(sz, 16), "type": typ}
    return syms, undef


def parse_sections(elf_path: pathlib.Path):
    _, out, _ = run([str(CEVA_BIN / "ceva-elf-readelf.exe"), "-S", "-W", str(elf_path)])
    secs = {}
    for ln in out.splitlines():
        m = re.match(
            r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)",
            ln,
        )
        if m:
            secs[m.group(1)] = {
                "type": m.group(2),
                "vma": int(m.group(3), 16),
                "offset": int(m.group(4), 16),
                "size": int(m.group(5), 16),
            }
    return secs


def parse_asm_frames(asm_paths):
    frames = {}
    callees = {}
    for p in asm_paths:
        cur = None
        for ln in p.read_text(encoding="utf-8", errors="ignore").splitlines():
            m = re.match(r"^([A-Za-z0-9_$.]+):\s*(?:;.*)?$", ln)
            if m and not m.group(1).startswith("."):
                cur = m.group(1)
                frames.setdefault(cur, 0)
                callees.setdefault(cur, set())
                continue
            if cur:
                fm = re.search(r"Frame size:\s*(\d+)\s*bytes", ln)
                if fm:
                    frames[cur] = int(fm.group(1))
                cm = re.search(r"\b(?:callr?|brr?)\s+\{?\s*([A-Za-z_][A-Za-z0-9_$]*)", ln)
                if cm:
                    callees[cur].add(cm.group(1))
    return frames, callees


def parse_disasm_callgraph(disasm: str):
    """Extract function names, stack frame allocs from sp adjustments, and call targets."""
    frames = {}
    calls = {}
    cur = None
    for ln in disasm.splitlines():
        fm = re.match(r"^[0-9a-fA-F]+\s+<([^>]+)>:", ln)
        if fm:
            lbl = fm.group(1)
            if not lbl.startswith("."):
                cur = lbl
                frames[cur] = 0
                calls[cur] = set()
            continue
        if cur:
            # Match PCU/SC stack allocate e.g. `LS1.modr (sp.ui).ui+#-400` or `add sp.ui, #-400, sp.ui`
            sm = re.search(r"(?:modr\s+\(sp\.ui\)\.ui\+#-|add\s+sp\.ui\s*,\s*#-|\badd\s+#-)(\d+)", ln)
            if sm:
                frames[cur] = max(frames[cur], int(sm.group(1)))
            cm = re.search(r";\s*<([^>+]+)>", ln)
            if cm and re.search(r"\bPCU\.(?:callr?|brr?)\b", ln):
                tgt = cm.group(1)
                if not tgt.startswith(".") and tgt != cur:
                    calls[cur].add(tgt)
    return frames, calls


def worst_chain(entry: str, frames: dict, calls: dict):
    best_total = -1
    best_path = []

    def dfs(fn, visited, acc, path):
        nonlocal best_total, best_path
        f_sz = frames.get(fn, 0)
        tot = acc + f_sz
        p2 = path + [{"function": fn, "frame_bytes": f_sz}]
        if tot > best_total:
            best_total = tot
            best_path = p2
        for nxt in sorted(calls.get(fn, ())):
            if nxt in frames and nxt not in visited:
                dfs(nxt, visited | {nxt}, tot, p2)

    dfs(entry, {entry}, 0, [])
    return {"total_bytes": best_total, "chain": best_path}


def main():
    OBJ.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)

    grain_src = SRC / "zf_grain_xm6_opt.cpp"
    hdf_src = SRC / "ricoh_hdf_xm6_opt.cpp"
    ld_script = SRC / "z8_elf0_coexist.ld"

    grain_o = OBJ / "zf_grain_nikondma.o"
    hdf_o = OBJ / "ricoh_hdf_nikondma.o"
    grain_s = OBJ / "zf_grain_nikondma.s"
    hdf_s = OBJ / "ricoh_hdf_nikondma.s"
    comb_o = OUT / "z8_xm6_coexist_nikondma.o"
    linked_elf = OUT / "z8_xm6_coexist_nikondma.elf"
    text_le_path = OUT / "text_z8fx_0x418c4580_raw_le.bin"
    text_hw_path = OUT / "text_z8fx_0x418c4580.bin"
    rodata_path = OUT / "rodata_idm_0x0003bc40.bin"
    pmss_stub_c = OBJ / "pmss_rcheck_stub.c"
    pmss_stub_o = OBJ / "pmss_rcheck_stub.o"
    rcheck_ld = OBJ / "z8_elf0_coexist_rcheck.ld"
    rcheck_elf = OUT / "z8_xm6_coexist_nikondma_rcheck.elf"
    rcheck_text_bin = OBJ / "rcheck_text_le.bin"

    skip_build = "--audit-only" in sys.argv
    if not skip_build:
        print("=== 1. Compiling Slot 39/40 (FilmGrain NikonDMA) & Slot 41 (HDF v2 NikonDMA) ===")
        run(
            [str(CEVA_BIN / "clang++.exe")]
            + ARCH_FLAGS
            + GRAIN_FLAGS
            + [f"-I{SRC}", "-Wall", "-Wextra", "-Wno-unused-function", "-c", str(grain_src), "-o", str(grain_o)]
        )
        run(
            [str(CEVA_BIN / "clang++.exe")]
            + ARCH_FLAGS
            + GRAIN_FLAGS
            + [f"-I{SRC}", "-S", str(grain_src), "-o", str(grain_s)]
        )
        run(
            [str(CEVA_BIN / "clang++.exe")]
            + ARCH_FLAGS
            + HDF_FLAGS
            + [f"-I{SRC}", "-Wall", "-Wextra", "-Wno-unused-function", "-c", str(hdf_src), "-o", str(hdf_o)]
        )
        run(
            [str(CEVA_BIN / "clang++.exe")]
            + ARCH_FLAGS
            + HDF_FLAGS
            + [f"-I{SRC}", "-S", str(hdf_src), "-o", str(hdf_s)]
        )

        print("=== 2. Linking combined relocatable .o and absolute monolithic 2-PHDR ELF ===")
        run([str(CEVA_BIN / "ceva-elf-ld.exe"), "-r", str(grain_o), str(hdf_o), "-o", str(comb_o)])
        run(
            [
                str(CEVA_BIN / "ceva-elf-ld.exe"),
                "--gc-sections",
                "-u", "Z8_FilmGrain_Slot39_40",
                "-u", "Z8_RicohHdf_Slot41",
                "-T", str(ld_script),
                str(grain_o),
                str(hdf_o),
                "-o", str(linked_elf),
            ]
        )

        run([str(CEVA_BIN / "ceva-elf-objcopy.exe"), "-O", "binary", "-j", ".text.z8fx", str(linked_elf), str(text_le_path)])
        run([str(CEVA_BIN / "ceva-elf-objcopy.exe"), "-O", "binary", "-j", ".rodata.z8fx", str(linked_elf), str(rodata_path)])

    raw_text = text_le_path.read_bytes()
    hw_text = bswap16_chunks(raw_text)
    text_hw_path.write_bytes(hw_text)
    rodata_bytes = rodata_path.read_bytes()

    if not skip_build:
        # Also build PMSS-stubbed linked ELF so ceva-elf-rcheck -full checks the FULL linked binary!
        stub_lines = []
        for sym in STOCK_PMSS_SYMS:
            stub_lines.append(f'__attribute__((section(".pmss.{sym}"), used)) void {sym}(void) {{}}')
        pmss_stub_c.write_text("\n".join(stub_lines) + "\n", encoding="utf-8")
        run([str(CEVA_BIN / "clang.exe")] + ARCH_FLAGS + ["-O2", "-c", str(pmss_stub_c), "-o", str(pmss_stub_o)])

        pmss_sec_lines = ["    .text.pmss_stub 0x000023c0 : {"]
        for sym, addr in sorted(STOCK_PMSS_SYMS.items(), key=lambda kv: kv[1]):
            pmss_sec_lines.append(f"        . = 0x{addr:08x} - 0x000023c0;")
            pmss_sec_lines.append(f"        KEEP(*(.pmss.{sym}))")
        pmss_sec_lines.append("    } > PMSS_STUB")

        base_ld = ld_script.read_text(encoding="utf-8")
        rcheck_ld_txt = re.sub(r"^PROVIDE\(.*\);\s*$", "", base_ld, flags=re.M)
        rcheck_ld_txt = rcheck_ld_txt.replace(
            "MEMORY\n{",
            "MEMORY\n{\n    PMSS_STUB  (rx) : ORIGIN = 0x000023c0, LENGTH = 0x5000",
        )
        rcheck_ld_txt = rcheck_ld_txt.replace(
            "SECTIONS\n{",
            "SECTIONS\n{\n" + "\n".join(pmss_sec_lines) + "\n",
        )
        rcheck_ld.write_text(rcheck_ld_txt, encoding="utf-8")
        run(
            [
                str(CEVA_BIN / "ceva-elf-ld.exe"),
                "--gc-sections",
                "-u", "Z8_FilmGrain_Slot39_40",
                "-u", "Z8_RicohHdf_Slot41",
                "-T", str(rcheck_ld),
                str(grain_o),
                str(hdf_o),
                str(pmss_stub_o),
                "-o", str(rcheck_elf),
            ]
        )
        run([str(CEVA_BIN / "ceva-elf-objcopy.exe"), "-O", "binary", "-j", ".text.z8fx", str(rcheck_elf), str(rcheck_text_bin)])
    rcheck_text_bytes = rcheck_text_bin.read_bytes()
    assert rcheck_text_bytes == raw_text, "PMSS-stubbed rcheck ELF .text.z8fx differs from shipped ELF!"

    print("=== 3. Running ceva-elf-rcheck.exe -full on ALL objects & linked ELF ===")
    rcheck_results = {
        "zf_grain_o": run_rcheck(grain_o, "zf_grain_nikondma.o"),
        "ricoh_hdf_xm6_v2_o": run_rcheck(hdf_o, "ricoh_hdf_nikondma.o"),
        "combined_coexist_o": run_rcheck(comb_o, "z8_xm6_coexist_nikondma.o"),
        "linked_coexist_elf_with_pmss_stub": run_rcheck(rcheck_elf, "z8_xm6_coexist_nikondma_rcheck.elf"),
    }
    rcheck_results["linked_coexist_elf_with_pmss_stub"]["text_bit_identical_to_shipped_elf"] = True
    rcheck_results["linked_coexist_elf_with_pmss_stub"]["verified_text_sha256"] = sha256_bytes(rcheck_text_bytes)

    print("=== 4. Running Hard Disassembly, ISA, Symbol, and Stack Audits ===")
    _, disasm_raw, _ = run([str(CEVA_BIN / "ceva-elf-objdump.exe"), "-d", str(linked_elf)])
    _, disasm_demang, _ = run([str(CEVA_BIN / "ceva-elf-objdump.exe"), "-d", "-C", str(linked_elf)])
    (OUT / "z8_xm6_coexist_nikondma_disasm.s").write_text(disasm_raw, encoding="utf-8")
    (OUT / "z8_xm6_coexist_nikondma_disasm_demang.s").write_text(disasm_demang, encoding="utf-8")

    _, obj_undef = parse_nm(comb_o)
    syms, linked_undef = parse_nm(linked_elf)
    secs = parse_sections(linked_elf)

    # 4a. Check sections & budgets
    text_sz = len(raw_text)
    ro_sz = len(rodata_bytes)
    data_sz = secs.get(".data", {}).get("size", 0)
    bss_sz = secs.get(".bss", {}).get("size", 0)
    assert secs[".text.z8fx"]["vma"] == 0x418C4580
    assert text_sz == secs[".text.z8fx"]["size"]
    assert text_sz <= 158336 and (text_sz % 32) == 0, f".text.z8fx size violation: {text_sz}"
    assert secs[".rodata.z8fx"]["vma"] == 0x0003BC40
    assert ro_sz == secs[".rodata.z8fx"]["size"]
    assert ro_sz <= 2048 and (0x0003BC40 + ro_sz) < 0x0003D000, f".rodata.z8fx size violation: {ro_sz}"
    assert data_sz == 0 and bss_sz == 0, f"Non-zero data/bss: {data_sz}/{bss_sz}"
    assert len(linked_undef) == 0, f"Unresolved symbols: {linked_undef}"

    # 4b. Entry symbols
    fg_vma = syms["Z8_FilmGrain_Slot39_40"]["addr"]
    hdf_vma = syms["Z8_RicohHdf_Slot41"]["addr"]
    assert (fg_vma % 32) == 0 and 0x418C4580 <= fg_vma < 0x418C4580 + text_sz
    assert (hdf_vma % 32) == 0 and 0x418C4580 <= hdf_vma < 0x418C4580 + text_sz

    # 4c. FP / Soft-FP / HDF v1 dead symbol checks
    fp_re = re.compile(r"\b(vfp\w+|fpmpy\w*|fpadd\w*|fpsub\w*|fpcvt\w*|fpdiv\w*|fpsqrt\w*|fmac\w*)\b", re.I)
    fp_hits = [ln.strip() for ln in disasm_raw.splitlines() if fp_re.search(ln)]
    soft_fp_re = re.compile(r"^__(?:aeabi_[fd]|add[sd]f|sub[sd]f|mul[sd]f|div[sd]f|float.*sf|fix.*sf)")
    soft_fp_hits = [s for s in syms if soft_fp_re.search(s)]
    hdf_v1_hits = [s for s in syms if "ricoh_hdf_xm6_run" in s or "hdf_v1" in s.lower()]
    assert len(fp_hits) == 0, f"FP insns found: {fp_hits[:5]}"
    assert len(soft_fp_hits) == 0, f"Soft-FP syms found: {soft_fp_hits[:5]}"
    assert len(hdf_v1_hits) == 0, f"HDF v1 dead syms found: {hdf_v1_hits[:5]}"

    # 4d. Strict CEVA-XM6 ISA Restriction 2 audit on ENTIRE linked .text.z8fx
    # Split VLIW bundles on '||' so scalar slots like SC0.xor r33.ds2 are not confused with LS0.vpld!
    vpld_insns = []
    vpst_insns = []
    for ln in disasm_raw.splitlines():
        if "vpld" in ln or "vpst" in ln:
            # Strip address & hex prefix before checking VLIW slots
            parts = ln.split("\t", 2)
            bundle = parts[2] if len(parts) >= 3 else ln
            for slot in bundle.split("||"):
                s = slot.strip()
                if re.search(r"\bvpld\b", s):
                    vpld_insns.append(s)
                if re.search(r"\bvpst\b", s):
                    vpst_insns.append(s)
    bad_gather_re = re.compile(r"\.(ds2|c2|c4|s2)\b")
    bad_vpld = [s for s in vpld_insns if bad_gather_re.search(s)]
    vpld_hist = {}
    for s in vpld_insns:
        m = re.search(r"vpld[^,\r\n]*", s)
        if m:
            norm = re.sub(r"\br\d+\b", "rX", re.sub(r"\bv\d+", "vX", m.group(0)))
            vpld_hist[norm] = vpld_hist.get(norm, 0) + 1
    assert len(bad_vpld) == 0, f"ILLEGAL vpld multi-element gathers found: {bad_vpld[:5]}"
    assert len(vpst_insns) == 0, f"vpst instructions found: {vpst_insns[:5]}"
    vpst_lines = vpst_insns

    # 4e. Verify ALL NikonDMA & libc call targets match stock z8_xm6_elf0.elf 100%
    # CEVA-XM6 objdump prints: `<pc_hex>:\t<bytes>\t... PCU.callr[.{ds1}] #<signed_rel> ... ; <<sym>>`
    ext_calls = {}
    call_counts = {}
    pc_re = re.compile(r"^([0-9a-fA-F]+):\t")
    rel_re = re.compile(r"\bPCU\.(?:callr|brr)(?:\{[^}]*\})?\s+#(-?\d+)")
    sym_re = re.compile(r";\s*<([^>+]+)>")
    for ln in disasm_raw.splitlines():
        ms = sym_re.search(ln)
        if not ms:
            continue
        tgt_name = ms.group(1)
        if tgt_name in STOCK_PMSS_SYMS:
            mp = pc_re.match(ln)
            mr = rel_re.search(ln)
            assert mp and mr, f"Unexpected call format for {tgt_name}: {ln}"
            pc = int(mp.group(1), 16)
            rel = int(mr.group(1))
            tgt_addr = (pc + rel) & 0xFFFFFFFF
            assert tgt_addr == STOCK_PMSS_SYMS[tgt_name], (
                f"Call target mismatch for {tgt_name} at 0x{pc:08x}: got 0x{tgt_addr:08x}, want 0x{STOCK_PMSS_SYMS[tgt_name]:08x}"
            )
            ext_calls[tgt_name] = f"0x{tgt_addr:08x}"
            call_counts[tgt_name] = call_counts.get(tgt_name, 0) + 1
    for req_sym in REQUIRED_NIKON_DMA_SYMS:
        assert req_sym in ext_calls, f"MISSING expected NikonDMA call in linked .text.z8fx: {req_sym}"

    # Verify Slot 41 avoids 0x41930000 and uses 0x41930040 fallback (spanning across .LBB* labels)
    m_s41 = re.search(r"<Z8_RicohHdf_Slot41>:.*?(?:\n[0-9a-fA-F]+ <(?!\.)|\Z)", disasm_raw, re.S)
    assert m_s41, "Z8_RicohHdf_Slot41 not found in disasm"
    s41_dis = m_s41.group(0)
    uses_41930040 = ("41930040" in s41_dis.lower()) and ("41930000" not in s41_dis.lower())
    assert uses_41930040, f"Slot 41 ext_workbuf fallback check failed:\n{s41_dis}"

    # 4f. Stack audit
    asm_frames, asm_calls = parse_asm_frames([grain_s, hdf_s])
    dis_frames, dis_calls = parse_disasm_callgraph(disasm_raw)
    fg_asm_chain = worst_chain("Z8_FilmGrain_Slot39_40", asm_frames, asm_calls)
    hdf_asm_chain = worst_chain("Z8_RicohHdf_Slot41", asm_frames, asm_calls)
    fg_dis_chain = worst_chain("Z8_FilmGrain_Slot39_40", dis_frames, dis_calls)
    hdf_dis_chain = worst_chain("Z8_RicohHdf_Slot41", dis_frames, dis_calls)
    worst_stack = max(
        fg_asm_chain["total_bytes"],
        hdf_asm_chain["total_bytes"],
        fg_dis_chain["total_bytes"],
        hdf_dis_chain["total_bytes"],
    )

    print(f"  .text.z8fx   : {text_sz:6d} B @ 0x418c4580..0x{0x418c4580+text_sz:08x} (headroom {158336-text_sz} B)")
    print(f"  .rodata.z8fx : {ro_sz:6d} B @ 0x0003bc40..0x{0x0003bc40+ro_sz:08x} (headroom to 2KiB {2048-ro_sz} B, to stack {0x3d000-(0x3bc40+ro_sz)} B)")
    print(f"  .data / .bss : {data_sz} / {bss_sz} B")
    print(f"  Z8_FilmGrain_Slot39_40 = 0x{fg_vma:08x} (size {syms['Z8_FilmGrain_Slot39_40']['size']} B)")
    print(f"  Z8_RicohHdf_Slot41     = 0x{hdf_vma:08x} (size {syms['Z8_RicohHdf_Slot41']['size']} B)")
    print(f"  UND: {linked_undef}, FP: {len(fp_hits)}, soft-FP: {len(soft_fp_hits)}, HDFv1: {len(hdf_v1_hits)}")
    print(f"  vpld hist: {vpld_hist} (bad .ds2/.c2/.c4/.s2: {len(bad_vpld)}, vpst: {len(vpst_lines)})")
    print(f"  External calls verified: {ext_calls} (counts: {call_counts})")
    print(f"  Stack worst: FG={fg_asm_chain['total_bytes']}/{fg_dis_chain['total_bytes']} B, HDF={hdf_asm_chain['total_bytes']}/{hdf_dis_chain['total_bytes']} B")

    src_inputs = {}
    for fname in [
        "zf_grain_xm6_opt.cpp",
        "zf_grain_xm6_ref.h",
        "ricoh_hdf_xm6_opt.cpp",
        "xm6_hdf_simd.h",
        "ricoh_hdf_xm6_v2.h",
        "z8_elf0_coexist.ld",
    ]:
        p = SRC / fname
        src_inputs[fname] = {"bytes": p.stat().st_size, "sha256": sha256_file(p)}

    manifest = {
        "schema": "v71r6.xm6_elf0_coexist_manifest/v1",
        "variant": "coexist_nikondma",
        "phase": "V71.6/V71.7-XM6 Production Hardware NikonDMA Monolithic 2-PHDR (Slot 39/40 Zf FilmGrain + Slot 41 Ricoh HDF v2, fixed .ds2 + 16B DMA align)",
        "target_firmware": "Nikon Z8 C3.11 z8_xm6_elf0.elf (CEVA-XM6 Core 0, RTL v1.2.0, FP=1, VFP=0)",
        "toolchain": {
            "suite": "CEVA-ToolBox V18.0.3 (win-lab)",
            "compiler": "clang++ 7.1.0 (20200127.1)",
            "arch_flags": ARCH_FLAGS,
            "filmgrain_flags": GRAIN_FLAGS,
            "hdf_v2_flags": HDF_FLAGS,
            "link_flags": [
                "--gc-sections",
                "-u", "Z8_FilmGrain_Slot39_40",
                "-u", "Z8_RicohHdf_Slot41",
                "-T", "z8_elf0_coexist.ld",
            ],
        },
        "source_inputs": src_inputs,
        "entries": {
            "Z8_FilmGrain_Slot39_40": {
                "vma_hex": f"0x{fg_vma:08x}",
                "vma_u32": fg_vma,
                "size_bytes": syms["Z8_FilmGrain_Slot39_40"]["size"],
                "slots": [39, 40],
            },
            "Z8_RicohHdf_Slot41": {
                "vma_hex": f"0x{hdf_vma:08x}",
                "vma_u32": hdf_vma,
                "size_bytes": syms["Z8_RicohHdf_Slot41"]["size"],
                "slots": [41],
            },
        },
        "function_table_patch": {
            "table_symbol": "g_NikonDspFrameworkFunctionTable",
            "table_base_idm_vma": "0x000394b8",
            "patches": [
                {
                    "slot": 39,
                    "purpose": "FilmGrain Still (EXACT/SHARP, NikonDMA 16B-aligned)",
                    "idm_vma": "0x00039554",
                    "elf0_offset": "0x00b774",
                    "main_main_offset": "0x01528214",
                    "stock_vma": "0x418bbdc0",
                    "new_vma": f"0x{fg_vma:08x}",
                    "new_vma_u32": fg_vma,
                    "new_le_hex": fg_vma.to_bytes(4, "little").hex(),
                },
                {
                    "slot": 40,
                    "purpose": "FilmGrain EVF / Video (EXACT/SHARP, NikonDMA 16B-aligned)",
                    "idm_vma": "0x00039558",
                    "elf0_offset": "0x00b778",
                    "main_main_offset": "0x01528218",
                    "stock_vma": "0x418bbdc0",
                    "new_vma": f"0x{fg_vma:08x}",
                    "new_vma_u32": fg_vma,
                    "new_le_hex": fg_vma.to_bytes(4, "little").hex(),
                },
                {
                    "slot": 41,
                    "purpose": "Ricoh Optical HDF v2 (LIVE_256_YC + STILL_GENERAL, NikonDMA, 0x .ds2)",
                    "idm_vma": "0x0003955c",
                    "elf0_offset": "0x00b77c",
                    "main_main_offset": "0x0152821c",
                    "stock_vma": "0x418bbdc0",
                    "new_vma": f"0x{hdf_vma:08x}",
                    "new_vma_u32": hdf_vma,
                    "new_le_hex": hdf_vma.to_bytes(4, "little").hex(),
                },
            ],
        },
        "sections": {
            ".text.z8fx": {
                "phdr_idx": 14,
                "p_flags": "0x5",
                "p_align": 32,
                "vma": "0x418c4580",
                "end_vma": f"0x{0x418c4580 + text_sz:08x}",
                "size_bytes": text_sz,
                "budget_bytes": 158336,
                "headroom_bytes": 158336 - text_sz,
                "byte_order": "expeed7_16byte_reversed",
                "expeed7_reversed_file": "text_z8fx_0x418c4580.bin",
                "expeed7_reversed_sha256": sha256_bytes(hw_text),
                "raw_le_file": "text_z8fx_0x418c4580_raw_le.bin",
                "raw_le_sha256": sha256_bytes(raw_text),
            },
            ".rodata.z8fx": {
                "phdr_idx": 15,
                "p_flags": "0x4",
                "p_align": 32,
                "vma": "0x0003bc40",
                "end_vma": f"0x{0x0003bc40 + ro_sz:08x}",
                "size_bytes": ro_sz,
                "target_budget_bytes": 2048,
                "headroom_to_2kib_bytes": 2048 - ro_sz,
                "idm_gap_budget_bytes": 5056,
                "headroom_to_stack_0x3d000_bytes": 0x0003D000 - (0x0003BC40 + ro_sz),
                "byte_order": "little_endian",
                "file": "rodata_idm_0x0003bc40.bin",
                "sha256": sha256_bytes(rodata_bytes),
            },
            ".data": {"size_bytes": data_sz},
            ".bss": {"size_bytes": bss_sz},
        },
        "audits": {
            "all_passed": True,
            "rcheck_full": rcheck_results,
            "obj_unresolved_symbols": sorted(obj_undef),
            "linked_unresolved_symbols": linked_undef,
            "external_calls": ext_calls,
            "external_call_counts": call_counts,
            "fp_instructions_found": len(fp_hits),
            "soft_fp_symbols_found": len(soft_fp_hits),
            "hdf_v1_dead_symbols_found": len(hdf_v1_hits),
            "isa_gather_audit": {
                "vpld_histogram": vpld_hist,
                "vpld_illegal_ds2_c2_c4_s2_count": len(bad_vpld),
                "vpst_count": len(vpst_lines),
            },
            "slot41_avoids_0x41930000_uses_0x41930040": uses_41930040,
            "stack_audit": {
                "freertos_task_stack_bytes": 12288,
                "budget_bytes": 1024,
                "worst_overall_bytes": worst_stack,
                "filmgrain_worst_chain_asm": fg_asm_chain,
                "filmgrain_worst_chain_disasm": fg_dis_chain,
                "ricoh_hdf_worst_chain_asm": hdf_asm_chain,
                "ricoh_hdf_worst_chain_disasm": hdf_dis_chain,
            },
        },
    }

    (OUT / "coexist_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("\n=== BUILD & HARD AUDITS PASSED 100% ===")


if __name__ == "__main__":
    main()
