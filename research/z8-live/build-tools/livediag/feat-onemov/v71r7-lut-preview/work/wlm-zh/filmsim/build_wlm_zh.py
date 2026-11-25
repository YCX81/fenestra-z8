#!/usr/bin/env python3
"""Build localized filmsim.wlm (ZH-CN / ZH-TW / EN) for --target z8-c311 cleanly
in an isolated directory without touching fenestra-z8 or feat-filmsim-wlm."""
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
REPO_ROOT = Path("/Users/ycx/Nikon")
FENESTRA_ROOT = REPO_ROOT / "fenestra-z8"
ABI_V71_EL1 = HERE.parents[1] / "filmsim/src/include/z8_filmsim_abi.h"
ABI_LOCAL = HERE / "filmsim_abi.h"
OUT_DIR = HERE / "build"


def strip_c_comments_ws(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    return re.sub(r"\s+", " ", text).strip()


def extract_abi_struct_signature(text: str) -> str:
    idx = text.find("typedef struct {")
    if idx < 0:
        raise RuntimeError("Missing typedef struct in ABI header")
    return strip_c_comments_ws(text[idx:])


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
        for name in ("filmsim_abi.h", "filmsim_core.h", "module.c"):
            tf.add(HERE / name, arcname=f"modules/filmsim/{name}", recursive=False)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def main() -> int:
    # Verify frozen 64B Z8FilmSimConfig / Z8FilmSimStatus ABI layout matches V71 EL1 header
    abi_bytes = ABI_LOCAL.read_bytes()
    abi_local_text = abi_bytes.decode("utf-8")
    abi_el1_bytes = ABI_V71_EL1.read_bytes()
    abi_el1_text = abi_el1_bytes.decode("utf-8")
    if "Z8_STYLE_FUJI_ZF" not in abi_local_text:
        raise RuntimeError("Local filmsim_abi.h missing Z8_STYLE_FUJI_ZF")
    if extract_abi_struct_signature(abi_local_text) != extract_abi_struct_signature(abi_el1_text):
        raise RuntimeError("Z8FilmSimConfig/Z8FilmSimStatus struct ABI mismatch vs V71 EL1 header!")
    abi_v71_el1_byte_identical = (abi_bytes == abi_el1_bytes)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    remote_dir = f"/tmp/filmsim-wlm-zh-{uuid.uuid4().hex[:12]}"
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
        "modules/filmsim/module.c",
        "--id", "filmsim",
        "--target", "z8-c311",
        "--optimize", "s",
        "--output-dir", "build/filmsim-z8",
    ]
    proc = subprocess.run(cmd, cwd=workdir, capture_output=True, text=True, check=True)
    out = workdir / "build" / "filmsim-z8"

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
            for name in ("filmsim.wlm", "module.elf", "module.json")
        },
    }
finally:
    shutil.rmtree(workdir, ignore_errors=True)

payload["remote_dir_cleaned"] = not workdir.exists()
print(json.dumps(payload))
"""
    repl = {"__REMOTE_DIR__": repr(remote_dir), "__TAR_B64__": repr(tar_b64)}
    remote_script = re.sub(
        "|".join(map(re.escape, repl)),
        lambda m: repl[m.group(0)],
        remote_script,
    )
    compile(remote_script, "<remote-wlm-zh-build>", "exec")

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

    wlm_bytes = (OUT_DIR / "filmsim.wlm").read_bytes()
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
        raise RuntimeError("SHA-256 mismatch between module.json and filmsim.wlm")
    if mod_json["modules"][0]["bytes"] != len(wlm_bytes):
        raise RuntimeError("Byte size mismatch between module.json and filmsim.wlm")
    if preflight != mod_json["preflight"]:
        raise RuntimeError("Local validate_package mismatch against module.json preflight")
    if preflight["relocations"] != 0:
        raise RuntimeError(f"Expected 0 relocations, got {preflight['relocations']}")

    fp_re = re.compile(r"\b(?:fadd|fsub|fmul|fdiv|fmadd|fmsub|fnmadd|fnmsub|fneg|fabs|fsqrt|fcmp|fcmpe|fccmp|fcsel|fcvt[a-z]*|scvtf|ucvtf|frint[a-z]*|fmax[a-z]*|fmin[a-z]*|frecp[a-z]*|frsq[a-z]*)\b")
    simd_re = re.compile(r"\b(?:v\d+\.|[bhsdq]\d+\b|fmov|fadd|fsub|fmul|fdiv|scvtf|ucvtf|fcvtz|fcvt|fcmp|fmadd|fmsub|dup\s|ushll|sshll|uaddw|usubw|uqadd|uqsub|shrn|ld[1-4]\s*\{|st[1-4]\s*\{|movi\s+v)")
    fp_cnt = simd_cnt = insn_cnt = 0
    for line in result.get("objdump_d", "").splitlines():
        m_ins = re.match(r"^\s+[0-9a-f]+:\s+[0-9a-f]{8}\s+(.*)$", line)
        if not m_ins:
            continue
        insn_cnt += 1
        rest = m_ins.group(1)
        if fp_re.search(rest):
            fp_cnt += 1
        if simd_re.search(rest):
            simd_cnt += 1
    if fp_cnt != 0 or simd_cnt != 0:
        raise RuntimeError(f"Expected 0 FP and 0 SIMD insns in module.elf, got fp={fp_cnt}, simd={simd_cnt}")

    git_diff = subprocess.run(
        ["git", "-C", str(FENESTRA_ROOT), "diff", "--name-only", "--", "host", "include", "src", "modules"],
        capture_output=True,
        text=True,
        check=True,
    )
    if git_diff.stdout.strip():
        raise RuntimeError(f"Modified tracked SDK files in fenestra-z8: {git_diff.stdout}")

    abi_sha = hashlib.sha256(abi_bytes).hexdigest()
    summary = {
        "target": "z8-c311",
        "module_id": "filmsim",
        "wlm_path": str(OUT_DIR / "filmsim.wlm"),
        "elf_path": str(OUT_DIR / "module.elf"),
        "sha256": actual_sha,
        "bytes": len(wlm_bytes),
        "abi_sha256": abi_sha,
        "preflight": preflight,
        "undefined_symbols": result["undefined_symbols"],
        "readelf_relocs": result["readelf_relocs"].strip(),
        "elf_insns": insn_cnt,
        "elf_fp_insns": fp_cnt,
        "elf_simd_insns": simd_cnt,
        "remote_dir_cleaned": result["remote_dir_cleaned"],
        "abi_struct_layout_identical": True,
        "abi_v71_el1_byte_identical": abi_v71_el1_byte_identical,
        "fenestra_sdk_clean": True,
    }
    (OUT_DIR / "verify-report.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
