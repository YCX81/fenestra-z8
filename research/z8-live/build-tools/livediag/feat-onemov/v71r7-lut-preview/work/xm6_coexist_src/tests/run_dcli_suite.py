#!/usr/bin/env python3
"""Compile, link against EXACT shipped coexist_nikondma objects (zf_grain_nikondma.o
& ricoh_hdf_nikondma.o), and execute the FULL dcli.exe 0-diff + NikonDMA alignment/bounds
verification matrix for BOTH Slot 41 HDF (22 cases) and Slot 39/40 Zf FilmGrain (25 cases).
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

CEVA = Path(r"C:\CEVA\CEVA-ToolBox_Tools\cevatools\bin")
WORK = Path(r"C:\temp\z8_xm6_coexist_nikondma")
SRC = WORK / "src"
OBJ = WORK / "obj"
TESTS = WORK / "tests"
DCLI = CEVA / "dcli.exe"

ARCH = [
    "--target=cevaxm6-unknown-unknown-elf",
    "-mcpu=rtlv1.2.0",
    "-mfp=1",
    "-mvfp=0",
    "-mnonlinear=2",
]

PARAMS_XML = """<?xml version="1.0" encoding="utf-8"?>
<Params>
  <Category Name="Debug">
    <Attribute><Name>Calling convention check</Name><Value>false</Value></Attribute>
    <Attribute><Name>MSS memory access</Name><Value>true</Value></Attribute>
  </Category>
  <Category Name="Execution Mode">
    <Attribute><Name>Execution Mode</Name><Value>Performance Accurate Mode</Value></Attribute>
    <Attribute><Name>Program Arguments</Name><Value></Value></Attribute>
    <Attribute><Name>Multi-Core Run Mode</Name><Value>false</Value></Attribute>
  </Category>
  <Category Name="HW Configuration">
    <Attribute><Name>CEVA-Xtend DLL</Name><Value/></Attribute>
    <Attribute><Name>CEVA-Xtend Location</Name><Value/></Attribute>
  </Category>
</Params>
"""


def run(cmd, cwd=TESTS):
    r = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True)
    if r.returncode != 0:
        print("FAILED:", " ".join(str(c) for c in cmd))
        print(r.stdout)
        print(r.stderr)
        sys.exit(r.returncode)
    return r.stdout


def run_dcli(elf: Path, tag: str) -> str:
    xml_p = TESTS / f"_params_{tag}.xml"
    cli_p = TESTS / f"_run_{tag}.cli"
    xml_p.write_text(PARAMS_XML, encoding="utf-8")
    cli_p.write_text(f"load elf {elf}\ngo\nexit\n", encoding="utf-8")
    env = os.environ.copy()
    env["PATH"] = str(CEVA) + ";" + env.get("PATH", "")
    cmd = [
        str(DCLI),
        "-c", "CEVA-XM6",
        "-t", str(SRC / "xm6_z8_target.xml"),
        "-p", str(xml_p),
        "-s", str(cli_p),
        "--no-stdin",
    ]
    t0 = time.time()
    proc = subprocess.Popen(
        cmd, cwd=str(TESTS), env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )
    out_lines = []
    halted = False
    for line in proc.stdout:
        out_lines.append(line)
        if line.startswith("[HDF") or line.startswith("[GRN") or "SUMMARY" in line or "ZFG1 loaded" in line or "FATAL" in line or "EXP" in line or "DMA detail" in line:
            print(line, end="", flush=True)
        if "EndOfExecution" in line or "Branch to self --> program halted" in line:
            halted = True
            break
    try:
        proc.wait(timeout=2.0 if halted else 240.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5.0)
    dt = time.time() - t0
    print(f"  [dcli {tag}: {dt:.2f}s, halted={halted}]", flush=True)
    full = "".join(out_lines)
    if not halted:
        print("DCLI DID NOT HALT CLEANLY:\n", full[-2000:])
        sys.exit(1)
    return full


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    # 1. Compile nikondma_mock.c once
    print("=== Compiling nikondma_mock.c ===", flush=True)
    run([
        str(CEVA / "clang.exe"), *ARCH, "-O2", f"-I{TESTS}",
        "-c", str(TESTS / "nikondma_mock.c"), "-o", str(TESTS / "nikondma_mock.o"),
    ])

    summary = {"hdf": [], "grain": [], "hdf_passed": 0, "hdf_failed": 0, "grain_passed": 0, "grain_failed": 0}

    if mode in ("all", "grain"):
        print("\n================ SLOT 39/40 Zf FilmGrain NikonDMA (25 cases) ================", flush=True)
        grain_batches = [(0, 4), (4, 6), (6, 18), (18, 25)]
        for b0, b1 in grain_batches:
            tag = f"grain_{b0}_{b1}"
            obj_p = TESTS / f"{tag}.o"
            elf_p = TESTS / f"{tag}.elf"
            run([
                str(CEVA / "clang.exe"), *ARCH, "-O2",
                f"-I{SRC}", f"-I{TESTS}",
                f"-DCASE_START={b0}U", f"-DCASE_END={b1}U",
                "-c", str(TESTS / "xm6_grain_nikondma_test.c"), "-o", str(obj_p),
            ])
            run([
                str(CEVA / "clang.exe"), *ARCH, "-O2",
                "-Wl,-T", str(TESTS / "dcli_nikondma_test.ld"),
                str(obj_p), str(TESTS / "nikondma_mock.o"),
                str(OBJ / "zf_grain_nikondma.o"), str(OBJ / "ricoh_hdf_nikondma.o"),
                "-o", str(elf_p),
            ])
            out = run_dcli(elf_p, tag)
            m = re.search(r"GRAIN_NIKONDMA_SUMMARY \[\d+\.\.\d+\): PASS=(\d+) FAIL=(\d+)", out)
            if not m or int(m.group(2)) != 0 or int(m.group(1)) != (b1 - b0):
                print(f"GRAIN BATCH {tag} FAILED!\n", out[-2500:])
                sys.exit(1)
            summary["grain_passed"] += int(m.group(1))
            summary["grain_failed"] += int(m.group(2))
            for ln in out.splitlines():
                if ln.startswith("[GRN"):
                    summary["grain"].append(ln.strip())

    if mode in ("all", "hdf", "evf"):
        print("\n================ SLOT 41 Ricoh HDF v2 NikonDMA (26 cases) ================", flush=True)
        hdf_batches = [(22, 26)] if mode == "evf" else [(0, 8), (8, 15), (15, 19), (19, 22), (22, 26)]
        for b0, b1 in hdf_batches:
            tag = f"hdf_{b0}_{b1}"
            obj_p = TESTS / f"{tag}.o"
            elf_p = TESTS / f"{tag}.elf"
            run([
                str(CEVA / "clang.exe"), *ARCH, "-O2",
                f"-I{SRC}", f"-I{TESTS}",
                f"-DCASE_START={b0}U", f"-DCASE_END={b1}U",
                "-c", str(TESTS / "xm6_hdf_nikondma_test.c"), "-o", str(obj_p),
            ])
            run([
                str(CEVA / "clang.exe"), *ARCH, "-O2",
                "-Wl,-T", str(TESTS / "dcli_nikondma_test.ld"),
                str(obj_p), str(TESTS / "nikondma_mock.o"),
                str(OBJ / "zf_grain_nikondma.o"), str(OBJ / "ricoh_hdf_nikondma.o"),
                "-o", str(elf_p),
            ])
            out = run_dcli(elf_p, tag)
            m = re.search(r"HDF_NIKONDMA_SUMMARY \[\d+\.\.\d+\): PASS=(\d+) FAIL=(\d+)", out)
            if not m or int(m.group(2)) != 0 or int(m.group(1)) != (b1 - b0):
                print(f"HDF BATCH {tag} FAILED!\n", out[-2500:])
                sys.exit(1)
            summary["hdf_passed"] += int(m.group(1))
            summary["hdf_failed"] += int(m.group(2))
            for ln in out.splitlines():
                if ln.startswith("[HDF"):
                    summary["hdf"].append(ln.strip())

    (TESTS / "dcli_suite_report.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"\n=== ALL DCLI SUITES PASSED: GRAIN={summary['grain_passed']}/{summary['grain_passed']+summary['grain_failed']}, HDF={summary['hdf_passed']}/{summary['hdf_passed']+summary['hdf_failed']} ===")


if __name__ == "__main__":
    main()
