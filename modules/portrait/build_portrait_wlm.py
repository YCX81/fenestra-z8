#!/usr/bin/env python3
"""Build localized portrait.wlm (V71.7-r4, 5 controls with Debug Settings sub-page)
for --target z8-c311 on win-lab WSL and verify preflight, 0 relocations, 0 FP/SIMD."""
import base64
import hashlib
import io
import json
import re
import subprocess
import sys
import tarfile
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
NIKON_ROOT = Path("/Users/ycx/Nikon")
FENESTRA_ROOT = NIKON_ROOT / "fenestra-z8"
OUT_DIR = HERE / "build"


def build_tar_gz_b64() -> str:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for rel_dir in ("host", "include"):
            src_dir = FENESTRA_ROOT / rel_dir
            for path in sorted(src_dir.rglob("*")):
                if path.is_dir() or "__pycache__" in path.parts or path.suffix == ".pyc":
                    continue
                arcname = path.relative_to(FENESTRA_ROOT).as_posix()
                tf.add(path, arcname=arcname, recursive=False)
        tf.add(FENESTRA_ROOT / "src" / "memory.c", arcname="src/memory.c", recursive=False)
        tf.add(HERE / "module.c", arcname="modules/portrait/module.c", recursive=False)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def audit_fp_simd(objdump_d: str):
    fp_re = re.compile(
        r"\b(?:fadd|fsub|fmul|fdiv|fmadd|fmsub|fnmadd|fnmsub|fneg|fabs|fsqrt|fcmp|fcmpe|"
        r"fccmp|fcsel|fcvt[a-z]*|scvtf|ucvtf|frint[a-z]*|fmax[a-z]*|fmin[a-z]*|frecp[a-z]*|frsq[a-z]*)\b"
    )
    simd_re = re.compile(
        r"\b(?:v\d+\.|[bhsdq]\d+\b|fmov|fadd|fsub|fmul|fdiv|scvtf|ucvtf|fcvtz|fcvt|fcmp|"
        r"fmadd|fmsub|dup\s|ushll|sshll|uaddw|usubw|uqadd|uqsub|shrn|ld[1-4]\s*\{|st[1-4]\s*\{|movi\s+v)"
    )
    fp_cnt = simd_cnt = insn_cnt = 0
    for line in objdump_d.splitlines():
        m = re.match(r"^\s+[0-9a-f]+:\s+[0-9a-f]{8}\s+(.*)$", line)
        if not m:
            continue
        insn_cnt += 1
        rest = m.group(1)
        if fp_re.search(rest):
            fp_cnt += 1
        if simd_re.search(rest):
            simd_cnt += 1
    return insn_cnt, fp_cnt, simd_cnt


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    remote_dir = f"/tmp/portrait-wlm-zh-{uuid.uuid4().hex[:12]}"
    tar_b64 = build_tar_gz_b64()

    remote_script = """import base64, io, json, shutil, subprocess, tarfile
from pathlib import Path

workdir = Path(__REMOTE_DIR__)
if workdir.exists():
    shutil.rmtree(workdir)
workdir.mkdir(parents=True, exist_ok=False)

try:
    raw = base64.b64decode(__TAR_B64__)
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as tf:
        tf.extractall(workdir)

    cmd = [
        "python3", "host/build_sdk_module.py",
        "modules/portrait/module.c",
        "--id", "portrait",
        "--target", "z8-c311",
        "--optimize", "s",
        "--output-dir", "build/portrait-z8",
    ]
    proc = subprocess.run(cmd, cwd=workdir, capture_output=True, text=True, check=True)
    out = workdir / "build" / "portrait-z8"

    nm = subprocess.run(["llvm-nm", "-u", str(out / "module.elf")],
                        capture_output=True, text=True, check=True)
    undef = [line.strip() for line in nm.stdout.splitlines() if line.strip()]
    readelf_r = subprocess.run(["llvm-readelf", "-r", str(out / "module.elf")],
                               capture_output=True, text=True, check=True)
    objdump_d = subprocess.run(["llvm-objdump", "-d", str(out / "module.elf")],
                               capture_output=True, text=True, check=True)

    payload = {
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "undefined_symbols": undef,
        "readelf_relocs": readelf_r.stdout,
        "objdump_d": objdump_d.stdout,
        "files": {
            name: base64.b64encode((out / name).read_bytes()).decode("ascii")
            for name in ("portrait.wlm", "module.elf", "module.json")
        },
    }
finally:
    shutil.rmtree(workdir, ignore_errors=True)

payload["remote_dir_cleaned"] = not workdir.exists()
print(json.dumps(payload))
"""
    repl = {"__REMOTE_DIR__": repr(remote_dir), "__TAR_B64__": repr(tar_b64)}
    remote_script = re.sub("|".join(map(re.escape, repl)), lambda m: repl[m.group(0)], remote_script)
    compile(remote_script, "<remote-portrait-build>", "exec")

    proc = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "win-lab", "wsl.exe -- python3 -"],
        input=remote_script.encode("utf-8"),
        capture_output=True,
        timeout=120,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", errors="replace"))
        sys.stdout.write(proc.stdout.decode("utf-8", errors="replace"))
        raise RuntimeError(f"Remote WSL build failed with exit code {proc.returncode}")

    result = json.loads(proc.stdout.decode("utf-8"))
    if not result.get("remote_dir_cleaned"):
        raise RuntimeError(f"Remote directory {remote_dir} was not deleted")
    if result.get("undefined_symbols"):
        raise RuntimeError(f"Unexpected undefined ELF symbols: {result['undefined_symbols']}")

    for name, b64 in result["files"].items():
        (OUT_DIR / name).write_bytes(base64.b64decode(b64))

    wlm_bytes = (OUT_DIR / "portrait.wlm").read_bytes()
    mod_json = json.loads((OUT_DIR / "module.json").read_text(encoding="utf-8"))
    actual_sha = hashlib.sha256(wlm_bytes).hexdigest()

    sys.path.insert(0, str(FENESTRA_ROOT / "host"))
    from module_header import validate_package
    from platform_profiles import PROFILES

    identity = bytes.fromhex(PROFILES["z8-c311"]["module_identity"])
    preflight = validate_package(wlm_bytes, identity)

    if mod_json["target"] != "z8-c311":
        raise RuntimeError(f"Unexpected target: {mod_json['target']}")
    if mod_json["modules"][0]["sha256"] != actual_sha:
        raise RuntimeError("SHA-256 mismatch between module.json and portrait.wlm")
    if mod_json["modules"][0]["bytes"] != len(wlm_bytes):
        raise RuntimeError("Byte size mismatch between module.json and portrait.wlm")
    if preflight != mod_json["preflight"]:
        raise RuntimeError("Local validate_package mismatch against module.json preflight")
    if preflight["relocations"] != 0:
        raise RuntimeError(f"Expected 0 relocations, got {preflight['relocations']}")

    insn_cnt, fp_cnt, simd_cnt = audit_fp_simd(result.get("objdump_d", ""))
    if fp_cnt != 0 or simd_cnt != 0:
        raise RuntimeError(f"Expected 0 FP and 0 SIMD insns, got fp={fp_cnt}, simd={simd_cnt}")

    summary = {
        "target": "z8-c311",
        "module_id": "portrait",
        "wlm_path": str(OUT_DIR / "portrait.wlm"),
        "elf_path": str(OUT_DIR / "module.elf"),
        "sha256": actual_sha,
        "bytes": len(wlm_bytes),
        "preflight": preflight,
        "undefined_symbols": result["undefined_symbols"],
        "readelf_relocs": result["readelf_relocs"].strip(),
        "elf_insns": insn_cnt,
        "elf_fp_insns": fp_cnt,
        "elf_simd_insns": simd_cnt,
        "remote_dir_cleaned": result["remote_dir_cleaned"],
    }
    (OUT_DIR / "verify-report.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
