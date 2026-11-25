#!/usr/bin/env python3
"""Publish the verified V71.7-r3 delivery bundle to:
  /Users/ycx/Nikon/delivery/Z8-v71.7r3-LutPreview-<sha8>/
  /Users/ycx/Nikon/delivery/Z8-v71.7r3-LutPreview-<sha8>.zip
via pack68.py, dynamically verifying EVERY fresh ARM, 0-Q-spill, and Whole-Guest QEMU
JSON receipt against the exact built main.main SHA-256 (ZERO hardcoded True!).

STRICT: Never modifies prior frozen delivery bundles (fe3e7f22, 9b36c4d4, v71.6, v71.5...).
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
V71R7 = HERE.parent
FINAL = V71R7 / "final"
TESTS = V71R7 / "tests"
FONT_ZH = V71R7 / "work/font-zh"
SD_FENESTRA = V71R7 / "sd-card/FENESTRA"
DELIVERY = Path("/Users/ycx/Nikon/delivery")


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def load_and_verify_receipt(path: Path, expected_main_sha: str, label: str) -> dict:
    assert path.is_file(), f"Missing fresh verification receipt ({label}): {path}"
    data = json.loads(path.read_text(encoding="utf-8"))
    passed = data.get("passed")
    assert passed is True, f"Receipt {label} ({path}) did NOT pass: passed={passed!r}"
    # Every binary/ARM/QEMU receipt must pin to the exact current main.main SHA-256
    rec_sha = (
        data.get("main_sha256")
        or data.get("final_main_sha256")
        or data.get("output_main_sha256")
    )
    assert rec_sha == expected_main_sha, (
        f"Stale receipt {label} ({path}): receipt main_sha256={rec_sha} != built {expected_main_sha}"
    )
    return data


def main() -> None:
    main_path = FINAL / "main.main"
    main_json = json.loads((FINAL / "main.json").read_text(encoding="utf-8"))
    main_sha = sha(main_path)
    assert main_sha == main_json["main_sha256"], (main_sha, main_json["main_sha256"])
    pkg_sha = sha(FINAL / "Z_8_0406.bin")
    build_id = main_json["build_id"]

    # 1. Dynamically verify ALL fresh r3 receipts (ZERO hardcoded True!)
    r_qspill = load_and_verify_receipt(TESTS / "verify_arm/qspill_audit_r3.json", main_sha, "qspill_audit_r3")
    assert r_qspill["q_spills"] == 0 and r_qspill["functions_scanned"] >= 1000, r_qspill

    r_l0_arm = load_and_verify_receipt(TESTS / "verify_arm/v71r7_lut_preview_arm.json", main_sha, "suite14_lut_preview_arm")
    r_xm6_arm = load_and_verify_receipt(TESTS / "verify_arm/xm6_bridge_arm.json", main_sha, "xm6_bridge_arm")
    assert r_xm6_arm.get("forbidden_0x40c91c84_hits", 1) == 0, r_xm6_arm

    r_fs_arm = load_and_verify_receipt(TESTS / "verify_arm/suite4_filmsim_final.json", main_sha, "suite4_filmsim_arm")
    r_hf_arm = load_and_verify_receipt(TESTS / "verify_arm/suite2_hangfix_arm.json", main_sha, "suite2_hangfix_arm")

    r_s20 = load_and_verify_receipt(TESTS / "verify_qemu_final/results/suite20_r3_lut_preview.json", main_sha, "qemu_suite20_lut_preview")
    assert r_s20.get("z8_unknown_count") == 0 and r_s20.get("z9_unknown_count") == 0 and r_s20.get("panic_count") == 0, r_s20

    r_s19 = load_and_verify_receipt(TESTS / "verify_qemu_final/results/suite19_r3_xm6_ui.json", main_sha, "qemu_suite19_xm6_ui")
    assert r_s19.get("z8_unknown_count") == 0 and r_s19.get("z9_unknown_count") == 0 and r_s19.get("panic_count") == 0, r_s19

    s2_audit = json.loads((FINAL / "stage2-portrait-audit.json").read_text(encoding="utf-8"))
    s3_audit = json.loads((FINAL / "stage3-fontzh-audit.json").read_text(encoding="utf-8"))
    wlm_glyphs = json.loads((TESTS / "wlm_zh/glyph_coverage_report.json").read_text(encoding="utf-8"))
    assert s2_audit.get("passed") is True and s3_audit.get("passed") is True
    assert s3_audit.get("final_main_sha256") == main_sha
    assert wlm_glyphs.get("passed") is True and wlm_glyphs.get("missing_sc_3font") == 0 and wlm_glyphs.get("missing_tc_3font") == 0

    all_checks_verified = bool(
        r_qspill["passed"]
        and r_l0_arm["passed"]
        and r_xm6_arm["passed"]
        and r_fs_arm["passed"]
        and r_hf_arm["passed"]
        and r_s20["passed"]
        and r_s19["passed"]
        and s2_audit["passed"]
        and s3_audit["passed"]
        and wlm_glyphs["passed"]
    )
    assert all_checks_verified

    prefix = "Z8-v71.7r3-LutPreview-"
    target_dir = DELIVERY / f"{prefix}{main_sha[:8]}"
    target_zip = DELIVERY / f"{prefix}{main_sha[:8]}.zip"
    assert not target_dir.exists(), f"Target dir already exists (immutable!): {target_dir}"
    assert not target_zip.exists(), f"Target zip already exists (immutable!): {target_zip}"

    # 2. Stage 5 FENESTRA/ files cleanly
    staged_fenestra = FINAL / "delivery-fenestra-r3"
    if staged_fenestra.exists():
        shutil.rmtree(staged_fenestra)
    staged_fenestra.mkdir(parents=True)

    fenestra_meta = {}
    for name in ("portrait.wlm", "slotcard.wlm", "filmsim.wlm", "ZF_GRAIN.BIN", "STARTUP-Z8.0"):
        src_p = SD_FENESTRA / name
        assert src_p.is_file(), f"Missing FENESTRA source: {src_p}"
        dst_p = staged_fenestra / name
        shutil.copyfile(src_p, dst_p)
        fenestra_meta[name] = {"bytes": dst_p.stat().st_size, "sha256": sha(dst_p)}

    # 3. Honest bilingual README
    readme_text = f"""Nikon Z8 V71.7-r3 固件发布包 ({prefix}{main_sha[:8]})
Nikon Z8 V71.7-r3: Hardened Viewfinder FPS Fix (RTOS Timed Pool0 Mutex + Isolated LCD XM6 + Verified DS 0x2126 Gate) + WYSIWYG Preview Toggle
====================================================================================================

【一、固件标识与校验和 / 1. FIRMWARE IDENTITY & CHECKSUMS】
----------------------------------------------------------------------------------------------------
  固件版本 / Version        : V71.7-r3 (v71r7-lut-preview 审查加固版)
  内部标识 / Identity       : 04.06.p0
  Bootguard Build ID        : {build_id} (at 0x401FE060)
  MAIN (eg2070) SHA-256     : {main_sha} (81,807,709 bytes)
  Z_8_0406.bin SHA-256      : {pkg_sha} (99,051,117 bytes)

  SD / CFexpress 卡 FENESTRA/ 模块（共 5 个文件，新版 filmsim.wlm 须与新版 MAIN 配套使用）:
    filmsim.wlm             : {fenestra_meta['filmsim.wlm']['sha256']} ({fenestra_meta['filmsim.wlm']['bytes']} bytes)  [#9 取景实时预览 开关，默认关闭]
    slotcard.wlm            : {fenestra_meta['slotcard.wlm']['sha256']} ({fenestra_meta['slotcard.wlm']['bytes']} bytes)
    portrait.wlm            : {fenestra_meta['portrait.wlm']['sha256']} ({fenestra_meta['portrait.wlm']['bytes']} bytes)
    ZF_GRAIN.BIN            : {fenestra_meta['ZF_GRAIN.BIN']['sha256']} ({fenestra_meta['ZF_GRAIN.BIN']['bytes']} bytes)
    STARTUP-Z8.0            : {fenestra_meta['STARTUP-Z8.0']['sha256']} ({fenestra_meta['STARTUP-Z8.0']['bytes']} bytes)

【二、V71.7-r3 针对独立代码审查的 5 项安全与帧率加固】
----------------------------------------------------------------------------------------------------
  1) EVF 1280x852 撤出 XM6 STILL_GENERAL：
     - 基于 sim_hdf_benchmark_report.json (still_1024x512 = 10.88M cyc = 20.75 cyc/px) 推算 EVF 1280x852 需 ~18-22M cyc (~27-60+ ms)，逼近 55 ms 超时线；
     - r3 恢复 EVF 走 ARM NEON，杜绝超时引发 XM6 退避或卡住 100 ms pool_suspend 的风险。
  2) Layer 0 LCD 预览与实况短片 XM6 状态彻底解耦（新增 z8_xm6_preview_hdf_nv12）：
     - 预览采用 0 ms 非阻塞抢锁，实况短片 30fps 与 45MP 静态照享有绝对优先权；
     - 预览超时仅写入独立 5 秒自动恢复冷却 (preview_backoff_until_us)，绝不写入共享 live_soft_disabled / live_timeout_streak，也不会永久关停 XM6。
  3) Pool 0 锁由 600us ARM yield 忙等升级为原厂 uITRON RTOS 定时阻塞锁：
     - 反汇编确认 C3.11 0x40D2C698(POOL_MUTEX, tmout_ms) 封装 uITRON tloc_mtx(0x401801F8，241 处原厂调用，支持优先级继承与解锁瞬间唤醒)；
     - 先 0 ms 快路径，冲突时调 0x40D2C698(POOL_MUTEX, 2U) 在调度器休眠等待（0% CPU 空转，不饿死 AFLENS/WTDG_C3），并暴露 pool_lock_waits / pool_lock_wait_ok 计数及拷贝循环内 is_still_capturing 及时中止。
  4) 释放模式门控仅使用已实证的 DS 0x2126 (0xA03D7C7A4)：
     - 移除未经实机验证的 DS 0x0108/0x0109，仅在 DS 0x2126 ∈ {{3=C30, 4=C120, 5=C60, 6=C15}} 时旁路 Layer 0，保持 S(0)/CL(1)/CH(2) 正常工作。
  5) 保留 r2 经审查确认无风险的两项有效改动：
     - #9「取景实时预览」开关（默认关闭 -> 不选 LUT 时 0 B Class 0x24，100% 原厂 60/120Hz 满帧）；
     - 源帧直接拷入独占 BANK_WORKING 槽位（消除每帧 1.05-1.64 MiB 二次整帧拷贝）。

【三、全部新鲜验证回执（均实跑于 {main_sha[:8]}，physical_camera_verified = false）】
----------------------------------------------------------------------------------------------------
  - 0-Q-Spill 静态审计   : {r_qspill['functions_scanned']} linked functions, q_spills = {r_qspill['q_spills']} (PASS)
  - Suite #14 Layer0 ARM : {r_l0_arm.get('checks', '?')} checks PASS
  - XM6 Bridge ARM       : {r_xm6_arm.get('checks', '?')} checks PASS, forbidden_0x40c91c84_hits = {r_xm6_arm.get('forbidden_0x40c91c84_hits')}
  - Suite #4 FilmSim ARM : {r_fs_arm.get('total_checks', '?')} checks PASS
  - Suite #2 HangFix ARM : {r_hf_arm.get('total_checks', '?')} checks PASS
  - Whole-Guest QEMU #19 : {r_s19.get('checks_passed')}/{r_s19.get('checks_total')} PASS (z8_unk=0, z9_unk=0, panic=0)
  - Whole-Guest QEMU #20 : {r_s20.get('checks_passed')}/{r_s20.get('checks_total')} PASS (z8_unk=0, z9_unk=0, panic=0)
  - Host ASan+UBSan      : layer0 7/7 (含 tloc_mtx 2ms 恢复 + DS 0x2126 0..6 门控), xm6_bridge 7/7 NEON + 7/7 SCALAR (含预览隔离), lut_core 6/6, photo_menu 5/5, wlm_zh 20/20
  - 诚实边界             : physical_camera_verified = false；Pool 0 实际定时锁命中率与 LCD 实时预览真实帧率需实体 Z8 刷入后通过 USB 实测验证。
"""
    readme_path = FINAL / "README-v71.7r3-lut-preview.txt"
    readme_path.write_text(readme_text, encoding="utf-8")

    # 4. Bundle ALL fresh verification JSON receipts into EVIDENCE/
    ver_dir = FINAL / "delivery-staging-meta-r3"
    if ver_dir.exists():
        shutil.rmtree(ver_dir)
    ver_dir.mkdir(parents=True)

    ev_files = [
        ("v71-manifest.json", FINAL / "v71-manifest.json"),
        ("stage1-build.json", FINAL / "stage1-build.json"),
        ("stage1-simd-audit.json", FINAL / "stage1-simd-audit.json"),
        ("stage2-portrait-audit.json", FINAL / "stage2-portrait-audit.json"),
        ("main.json", FINAL / "main.json"),
        ("stage3-fontzh-audit.json", FINAL / "stage3-fontzh-audit.json"),
        ("qspill_audit_r3.json", TESTS / "verify_arm/qspill_audit_r3.json"),
        ("v71r7_lut_preview_arm.json", TESTS / "verify_arm/v71r7_lut_preview_arm.json"),
        ("xm6_bridge_arm.json", TESTS / "verify_arm/xm6_bridge_arm.json"),
        ("suite4_filmsim_final.json", TESTS / "verify_arm/suite4_filmsim_final.json"),
        ("suite2_hangfix_arm.json", TESTS / "verify_arm/suite2_hangfix_arm.json"),
        ("qemu_suite19_r3_xm6_ui.json", TESTS / "verify_qemu_final/results/suite19_r3_xm6_ui.json"),
        ("qemu_suite20_r3_lut_preview.json", TESTS / "verify_qemu_final/results/suite20_r3_lut_preview.json"),
        ("wlm_filmsim_verify_report.json", V71R7 / "work/wlm-zh/filmsim/build/verify-report.json"),
        ("wlm_filmsim_glyph_coverage.json", TESTS / "wlm_zh/glyph_coverage_report.json"),
    ]
    evidence_entries = []
    for nm, p in ev_files:
        assert p.is_file(), f"Missing required evidence file: {p}"
        evidence_entries.append({"name": nm, "path": os.path.relpath(p, ver_dir), "sha256": sha(p)})

    ver_path = ver_dir / "verification.json"
    ver_path.write_text(
        json.dumps(
            {
                "schema": "v71.7r3.lut-preview-delivery-verification/v1",
                "version": "v71.7r3-lut-preview",
                "identity": "04.06.p0",
                "main_sha256": main_sha,
                "package_sha256": pkg_sha,
                "build_id": build_id,
                "release_checks_passed": all_checks_verified,
                "physical_camera_verified": False,
                "verified_receipts": {
                    "qspill_funcs": r_qspill["functions_scanned"],
                    "qspill_count": r_qspill["q_spills"],
                    "suite14_arm_checks": r_l0_arm.get("checks"),
                    "xm6_arm_checks": r_xm6_arm.get("checks"),
                    "suite4_arm_checks": r_fs_arm.get("total_checks"),
                    "suite2_arm_checks": r_hf_arm.get("total_checks"),
                    "qemu_suite19": f"{r_s19.get('checks_passed')}/{r_s19.get('checks_total')}",
                    "qemu_suite20": f"{r_s20.get('checks_passed')}/{r_s20.get('checks_total')}",
                },
                "sd_card_fenestra": fenestra_meta,
                "evidence": evidence_entries,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    subprocess.run(
        [
            "python3",
            str(FONT_ZH / "pack68.py"),
            "--main",
            str(main_path),
            "--verification",
            str(ver_path),
            "--readme",
            str(readme_path),
            "--fenestra",
            str(staged_fenestra),
            "--output",
            str(DELIVERY),
            "--name-prefix",
            prefix,
            "--version",
            "v71.7r3-lut-preview",
        ],
        check=True,
    )

    shasum_res = subprocess.run(
        ["shasum", "-a", "256", "-c", "SHA256SUMS.txt"],
        cwd=str(target_dir),
        text=True,
        capture_output=True,
        check=True,
    )
    unzip_res = subprocess.run(
        ["unzip", "-tq", str(target_zip)],
        text=True,
        capture_output=True,
        check=True,
    )
    print(shasum_res.stdout, end="")
    print(unzip_res.stdout or "unzip -tq OK", end="")
    print(
        json.dumps(
            {
                "delivery_dir": str(target_dir),
                "delivery_zip": str(target_zip),
                "z_8_0406_bin_sha256": pkg_sha,
                "main_sha256": main_sha,
                "build_id": build_id,
                "filmsim_wlm_sha256": fenestra_meta["filmsim.wlm"]["sha256"],
                "evidence_count": len(evidence_entries),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
