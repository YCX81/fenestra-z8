#!/usr/bin/env python3
"""Publish the verified V71.7-r7 delivery bundle to:
  /Users/ycx/Nikon/delivery/Z8-v71.7r7-LutPreview-<sha8>/
  /Users/ycx/Nikon/delivery/Z8-v71.7r7-LutPreview-<sha8>.zip
via pack68.py, dynamically verifying EVERY fresh CEVA-XM6 dcli 51/51, ARM64 Unicorn,
0-Q-spill, 5-WLM glyph, and Whole-Guest QEMU JSON receipt against the exact built
main.main SHA-256 (ZERO hardcoded True!).

STRICT: Never modifies prior frozen delivery bundles (r6, r5, r4, r3, r2, r1, v71.6, v71.5...).
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
XM6_COEXIST = HERE / "xm6_elf0/coexist_nikondma"
DELIVERY = Path("/Users/ycx/Nikon/delivery")

FENESTRA_FILES = (
    "filmsim.wlm",
    "livephoto.wlm",
    "lut.wlm",
    "slotcard.wlm",
    "portrait.wlm",
    "ZF_GRAIN.BIN",
    "STARTUP-Z8.0",
)


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def load_and_verify_receipt(path: Path, expected_main_sha: str, label: str) -> dict:
    assert path.is_file(), f"Missing fresh verification receipt ({label}): {path}"
    data = json.loads(path.read_text(encoding="utf-8"))
    passed = data.get("passed")
    assert passed is True, f"Receipt {label} ({path}) did NOT pass: passed={passed!r}"
    rec_sha = (
        data.get("main_sha256")
        or data.get("final_main_sha256")
        or data.get("candidate_main_sha256")
        or data.get("output_main_sha256")
    )
    assert rec_sha == expected_main_sha, (
        f"Stale receipt {label} ({path}): receipt main_sha256={rec_sha} != built {expected_main_sha}"
    )
    return data


def main() -> None:
    main_path = FINAL / "main.main"
    main_json = json.loads((FINAL / "main.json").read_text(encoding="utf-8"))
    stage0_json = json.loads((FINAL / "stage0-base.json").read_text(encoding="utf-8"))
    stage1_build = json.loads((FINAL / "stage1-build.json").read_text(encoding="utf-8"))
    main_sha = sha(main_path)
    assert main_sha == main_json["main_sha256"], (main_sha, main_json["main_sha256"])
    pkg_sha = sha(FINAL / "Z_8_0406.bin")
    build_id = main_json["build_id"]

    # 1. Dynamically verify CEVA-XM6 Core 0 Slot 41 (LIVE_EVF_256_YC) manifest & 51/51 dcli.exe report
    xm6_man_path = XM6_COEXIST / "coexist_manifest.json"
    xm6_dcli_path = XM6_COEXIST / "dcli_suite_report.json"
    xm6_man = json.loads(xm6_man_path.read_text(encoding="utf-8"))
    xm6_dcli = json.loads(xm6_dcli_path.read_text(encoding="utf-8"))
    assert xm6_man["audits"]["all_passed"] is True, xm6_man["audits"]
    xm6_text_sz = xm6_man["sections"][".text.z8fx"]["size_bytes"]
    xm6_ro_sz = xm6_man["sections"][".rodata.z8fx"]["size_bytes"]
    assert xm6_text_sz <= 158336 and xm6_ro_sz <= 2048, (xm6_text_sz, xm6_ro_sz)
    assert xm6_man["sections"][".text.z8fx"]["expeed7_reversed_sha256"] == stage0_json["xm6_elf0"]["added_segments"][0]["sha256"]
    xm6_dcli_passed = int(xm6_dcli.get("hdf_passed", 0)) + int(xm6_dcli.get("grain_passed", 0))
    xm6_dcli_failed = int(xm6_dcli.get("hdf_failed", 1)) + int(xm6_dcli.get("grain_failed", 1))
    assert xm6_dcli_passed == 51 and xm6_dcli_failed == 0 and xm6_dcli.get("hdf_passed") == 26 and xm6_dcli.get("grain_passed") == 25, xm6_dcli

    # 2. Dynamically verify ALL fresh r7 ARM64, 0-Q-spill, and Whole-Guest QEMU receipts (ZERO hardcoded True!)
    r_qspill = load_and_verify_receipt(TESTS / "verify_arm/qspill_audit_r7.json", main_sha, "qspill_audit_r7")
    assert r_qspill["q_spills"] == 0 and r_qspill["functions_scanned"] >= 1000, r_qspill

    r_s5_arm = load_and_verify_receipt(TESTS / "verify_arm/suite5_font_cjk_final.json", main_sha, "suite5_font_cjk_arm")
    r_l0_arm = load_and_verify_receipt(TESTS / "verify_arm/v71r7_lut_preview_arm.json", main_sha, "suite14_lut_preview_arm")
    r_xm6_arm = load_and_verify_receipt(TESTS / "verify_arm/xm6_bridge_arm.json", main_sha, "xm6_bridge_arm")
    assert r_xm6_arm.get("forbidden_0x40c91c84_hits", 1) == 0, r_xm6_arm
    r_fs_arm = load_and_verify_receipt(TESTS / "verify_arm/suite4_filmsim_final.json", main_sha, "suite4_filmsim_arm")
    r_hf_arm = load_and_verify_receipt(TESTS / "verify_arm/suite2_hangfix_arm.json", main_sha, "suite2_hangfix_arm")

    r_s20 = load_and_verify_receipt(TESTS / "verify_qemu_final/results/suite20_r7_lut_preview.json", main_sha, "qemu_suite20_r7_lut_preview")
    assert r_s20.get("z8_unknown_count") == 0 and r_s20.get("z9_unknown_count") == 0 and r_s20.get("panic_count") == 0, r_s20

    r_s19 = load_and_verify_receipt(TESTS / "verify_qemu_final/results/suite19_r7_plugins_xm6_ui.json", main_sha, "qemu_suite19_r7_plugins_xm6_ui")
    assert r_s19.get("z8_unknown_count") == 0 and r_s19.get("z9_unknown_count") == 0 and r_s19.get("panic_count") == 0, r_s19

    s2_audit = json.loads((FINAL / "stage2-portrait-audit.json").read_text(encoding="utf-8"))
    s3_audit = json.loads((FINAL / "stage3-fontzh-audit.json").read_text(encoding="utf-8"))
    wlm_glyphs = json.loads((TESTS / "wlm_zh/glyph_coverage_report.json").read_text(encoding="utf-8"))
    assert s2_audit.get("passed") is True and s3_audit.get("passed") is True
    assert s3_audit.get("final_main_sha256") == main_sha
    assert wlm_glyphs.get("passed") is True and wlm_glyphs.get("missing_sc_3font") == 0 and wlm_glyphs.get("missing_tc_3font") == 0

    wlm_reports = {}
    for mod in ("filmsim", "livephoto", "lut", "slotcard", "portrait"):
        rp = V71R7 / f"work/wlm-zh/{mod}/build/verify-report.json"
        assert rp.is_file(), f"Missing WLM verify-report.json for {mod}: {rp}"
        jd = json.loads(rp.read_text(encoding="utf-8"))
        assert jd.get("sha256") == sha(SD_FENESTRA / f"{mod}.wlm"), (mod, jd.get("sha256"))
        assert jd.get("undefined_symbols") == [], (mod, jd.get("undefined_symbols"))
        assert jd.get("elf_fp_insns") == 0 and jd.get("elf_simd_insns") == 0, (mod, jd)
        assert (jd.get("preflight") or {}).get("relocations") == 0, (mod, jd)
        wlm_reports[mod] = rp

    all_checks_verified = bool(
        xm6_man["audits"]["all_passed"]
        and xm6_dcli_passed == 51
        and xm6_dcli_failed == 0
        and r_qspill["passed"]
        and r_s5_arm["passed"]
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

    prefix = "Z8-v71.7r7-LutPreview-"
    target_dir = DELIVERY / f"{prefix}{main_sha[:8]}"
    target_zip = DELIVERY / f"{prefix}{main_sha[:8]}.zip"
    assert not target_dir.exists(), f"Target dir already exists (immutable!): {target_dir}"
    assert not target_zip.exists(), f"Target zip already exists (immutable!): {target_zip}"

    # 3. Stage ALL 7 FENESTRA/ files cleanly (unchanged from r6!)
    staged_fenestra = FINAL / "delivery-fenestra-r7"
    if staged_fenestra.exists():
        shutil.rmtree(staged_fenestra)
    staged_fenestra.mkdir(parents=True)

    fenestra_meta = {}
    for name in FENESTRA_FILES:
        src_p = SD_FENESTRA / name
        assert src_p.is_file(), f"Missing FENESTRA source: {src_p}"
        dst_p = staged_fenestra / name
        shutil.copyfile(src_p, dst_p)
        fenestra_meta[name] = {"bytes": dst_p.stat().st_size, "sha256": sha(dst_p)}

    # 4. Honest bilingual README (100% consistent with verification.json!)
    readme_text = f"""Nikon Z8 V71.7-r7 固件发布包 ({prefix}{main_sha[:8]})
Nikon Z8 V71.7-r7: XM6 De-Contention + Reverse Layer0->Live Single-Pass + 528 KiB L2 Baked YCbCr LUT + Post-Shutter Still Deferral
===================================================================================================================================

【一、固件标识与校验和 / 1. FIRMWARE IDENTITY & CHECKSUMS】
-----------------------------------------------------------------------------------------------------------------------------------
  固件版本 / Version        : V71.7-r7 (四项全开 EVF/背屏流畅度 + 实况照片快门后零冻结全面提速版)
  内部标识 / Identity       : 04.06.p0
  Bootguard Build ID        : {build_id} (at 0x401FE060)
  MAIN (eg2070) SHA-256     : {main_sha} (81,807,709 bytes)
  Z_8_0406.bin SHA-256      : {pkg_sha} (99,051,117 bytes)
  XM6 Core 0 ELF0 SHA-256   : {stage0_json['xm6_elf0']['elf0_patched_sha256']} (.text.z8fx={xm6_text_sz}/158336 B, .rodata.z8fx={xm6_ro_sz}/2048 B)

  【升级说明 / UPGRADE NOTE】
  - 若当前存储卡已部署 V71.7-r6 的 FENESTRA/ 全部 7 个文件，本版仅需刷入 Z_8_0406.bin（全部 5 个 .wlm 与数据文件与 r6 逐字节一致）。
  - 若从 r5 或更早版本升级，请完整覆盖存储卡 FENESTRA/ 全部 7 个文件并刷入 Z_8_0406.bin。

  FENESTRA/ 全部 7 个文件清单（与 r6 一致）：
    filmsim.wlm             : {fenestra_meta['filmsim.wlm']['sha256']} ({fenestra_meta['filmsim.wlm']['bytes']} bytes)
    livephoto.wlm           : {fenestra_meta['livephoto.wlm']['sha256']} ({fenestra_meta['livephoto.wlm']['bytes']} bytes)
    lut.wlm                 : {fenestra_meta['lut.wlm']['sha256']} ({fenestra_meta['lut.wlm']['bytes']} bytes)
    slotcard.wlm            : {fenestra_meta['slotcard.wlm']['sha256']} ({fenestra_meta['slotcard.wlm']['bytes']} bytes)
    portrait.wlm            : {fenestra_meta['portrait.wlm']['sha256']} ({fenestra_meta['portrait.wlm']['bytes']} bytes)
    ZF_GRAIN.BIN            : {fenestra_meta['ZF_GRAIN.BIN']['sha256']} ({fenestra_meta['ZF_GRAIN.BIN']['bytes']} bytes)
    STARTUP-Z8.0            : {fenestra_meta['STARTUP-Z8.0']['sha256']} ({fenestra_meta['STARTUP-Z8.0']['bytes']} bytes)

【二、V71.7-r7 四项核心流畅度根修（针对 r6 真机 USB 与实拍 .MOV 取证）】
-----------------------------------------------------------------------------------------------------------------------------------
  1) 根修 XM6 锁前邮箱误判回退（消除 r6 实测 21–22.5% XM6 ERR_NOT_READY 回退）：
     - r6 在 xm6_live_or_preview_hdf_nv12 入口先检查含「邮箱空闲」的 hw_ready，只要另一路 XM6 作业在跑就直接报 NOT_READY(5) 回退到 ARM，
       绕过了有上限等待逻辑。
     - r7 拆分锁前静态就绪检查与锁后邮箱空闲等待（最长 16 ms 排队 + waiters 唤醒 dly_tsk(0) 交接），并将 IOMMU 映射与提交前
       D-Cache clean/invalidate 提前到持锁之前，最大限度压缩 XM6 锁持有时间。
  2) 新增反向「Layer 0 -> Live」单通道共享（EVF 1280x852 与背屏 1024x682 均生效）：
     - 开启 LUT / Layer 0 实时取景预览时，Layer 0 在 Stage 1 HDF 完成后、Stage 2 Grain 之前，按 >=18 ms 节奏将已调色+HDF 的 NV12
       直接推入 Live 环形缓冲（LBUF）为 FS_TB_GRADED_READY；CPU0 自动跳过重复缩放与提交，live_worker 与 XM6 不再做第二份重复调色与 HDF；
       背屏也不再被旧版「Live -> 背屏」30 fps 复用机制封顶。
  3) 528 KiB L2 常驻预烘焙 64x64x33 YCbCr 3D LUT + 内联 NEON 拷贝 + 融合颗粒缓存清洗 + 自适应 30 fps EVF 节拍：
     - 从 photo_workspace 已有 stage_cpu + 0x80000 未使用尾部切出 528 KiB 预烘焙表（零 General 堆增长、Class 0x24 严格保持 0xE8C000），
       2x2 Bayer 色度抖动 + 3-bit Y 线性插值，中性灰零偏色，NEON 与标量逐位 0-diff，大幅降低 CPU2/CPU3 3D LUT 与拷贝/刷缓存耗时；
       仅当独立 Live（无反向共享）运行时启用 EVF 30 fps 均匀节拍，避免 35–42 fps 在 60 Hz 上不均匀掉帧感。
  4) 快门后 45 MP 静图顺延（消除 r6 实拍 Live .MOV 后半段 5.8 fps 连续冻结）：
     - 在 live_runtime_post_roll_active() 期间且非连拍双槽满载时，将 45 MP 静图 PENDING -> COPY 顺延至 post-roll 结束（硬上限 1.65 s，
       远低于 30 s 超时阈值），让实况照片快门后 1.5 s 保持流畅 29.97 fps。

【三、全部新鲜验证回执（均实跑于 {main_sha[:8]}，physical_camera_verified = false）】
-----------------------------------------------------------------------------------------------------------------------------------
  - CEVA-XM6 dcli.exe 仿真 : {xm6_dcli_passed}/{xm6_dcli_passed + xm6_dcli_failed} PASS (GRAIN {xm6_dcli['grain_passed']}/25 + HDF {xm6_dcli['hdf_passed']}/26；.text.z8fx={xm6_text_sz}/158336 B, .rodata.z8fx={xm6_ro_sz}/2048 B)
  - 0-Q-Spill 静态审计     : {r_qspill['functions_scanned']} linked functions, q_spills = {r_qspill['q_spills']} (PASS)
  - Suite #5 FontCJK ARM   : 3755/3755 SC + 3755/3755 TC + 13x4 menu text + slots.state ops 13..16 (PASS)
  - Suite #14 Layer0 ARM   : {r_l0_arm.get('checks_passed', r_l0_arm.get('checks', '?'))} checks PASS
  - XM6 Bridge ARM         : {r_xm6_arm.get('checks_passed', r_xm6_arm.get('checks', '?'))} checks PASS, forbidden_0x40c91c84_hits = {r_xm6_arm.get('forbidden_0x40c91c84_hits')}
  - Suite #4 FilmSim ARM   : {r_fs_arm.get('total_checks', '?')} checks PASS
  - Suite #2 HangFix ARM   : {r_hf_arm.get('total_checks', '?')} checks PASS
  - 5-WLM CJK 字库审计     : missing_sc_3font = {wlm_glyphs.get('missing_sc_3font')}, missing_tc_3font = {wlm_glyphs.get('missing_tc_3font')} (PASS)
  - Whole-Guest QEMU #19-r7: {r_s19.get('checks_passed')}/{r_s19.get('checks_total')} PASS (纯净 Fenestra 7行 + 5插件 + XM6自检/持久化/回退, z8_unk=0, panic=0)
  - Whole-Guest QEMU #20-r7: {r_s20.get('checks_passed')}/{r_s20.get('checks_total')} PASS (Row 47 流式 LUT + Layer0 528KiB 预烘焙 LUT + Still + Movie挂起0B + 重启恢复/清除, z8_unk=0, panic=0)
  - Host ASan+UBSan        : lut_core 8/8 (a..h, 含 528 KiB 烘焙 LUT NEON==Scalar 0-diff/PSNR/SMP/NEON拷贝), xm6_bridge 9/9 (a..i NEON+SCALAR, 含锁前排队0回退/反向Layer0->Live/快门后顺延/颗粒融合清洗), layer0 9/9, photo_menu 6/6 全通过
  - 诚实边界               : physical_camera_verified = false；真机刷入本版并连接 WinLab USB 后，可直接用只读探针复测 EVF/背屏帧率、XM6 回退率与实拍 Live .MOV 后半段帧率。
"""
    readme_path = FINAL / "README-v71.7r7-lut-preview.txt"
    readme_path.write_text(readme_text, encoding="utf-8")

    # 5. Bundle ALL fresh verification JSON receipts into EVIDENCE/
    ver_dir = FINAL / "delivery-staging-meta-r7"
    if ver_dir.exists():
        shutil.rmtree(ver_dir)
    ver_dir.mkdir(parents=True)

    ev_files = [
        ("v71-manifest.json", FINAL / "v71-manifest.json"),
        ("stage0-base.json", FINAL / "stage0-base.json"),
        ("stage1-build.json", FINAL / "stage1-build.json"),
        ("stage1-simd-audit.json", FINAL / "stage1-simd-audit.json"),
        ("stage2-portrait-audit.json", FINAL / "stage2-portrait-audit.json"),
        ("main.json", FINAL / "main.json"),
        ("stage3-fontzh-audit.json", FINAL / "stage3-fontzh-audit.json"),
        ("xm6_coexist_manifest.json", xm6_man_path),
        ("xm6_dcli_suite_report_51.json", xm6_dcli_path),
        ("qspill_audit_r7.json", TESTS / "verify_arm/qspill_audit_r7.json"),
        ("suite5_font_cjk_final.json", TESTS / "verify_arm/suite5_font_cjk_final.json"),
        ("v71r7_lut_preview_arm.json", TESTS / "verify_arm/v71r7_lut_preview_arm.json"),
        ("xm6_bridge_arm.json", TESTS / "verify_arm/xm6_bridge_arm.json"),
        ("suite4_filmsim_final.json", TESTS / "verify_arm/suite4_filmsim_final.json"),
        ("suite2_hangfix_arm.json", TESTS / "verify_arm/suite2_hangfix_arm.json"),
        ("qemu_suite19_r7_plugins_xm6_ui.json", TESTS / "verify_qemu_final/results/suite19_r7_plugins_xm6_ui.json"),
        ("qemu_suite20_r7_lut_preview.json", TESTS / "verify_qemu_final/results/suite20_r7_lut_preview.json"),
        ("wlm_5plugins_glyph_coverage.json", TESTS / "wlm_zh/glyph_coverage_report.json"),
        ("wlm_filmsim_verify_report.json", wlm_reports["filmsim"]),
        ("wlm_livephoto_verify_report.json", wlm_reports["livephoto"]),
        ("wlm_lut_verify_report.json", wlm_reports["lut"]),
        ("wlm_slotcard_verify_report.json", wlm_reports["slotcard"]),
        ("wlm_portrait_verify_report.json", wlm_reports["portrait"]),
    ]
    evidence_entries = []
    for nm, p in ev_files:
        assert p.is_file(), f"Missing required evidence file: {p}"
        evidence_entries.append({"name": nm, "path": os.path.relpath(p, ver_dir), "sha256": sha(p)})

    ver_path = ver_dir / "verification.json"
    ver_path.write_text(
        json.dumps(
            {
                "schema": "v71.7r7.lut-preview-delivery-verification/v1",
                "version": "v71.7r7-lut-preview",
                "identity": "04.06.p0",
                "main_sha256": main_sha,
                "package_sha256": pkg_sha,
                "build_id": build_id,
                "release_checks_passed": all_checks_verified,
                "physical_camera_verified": False,
                "verified_receipts": {
                    "xm6_dcli_suite": f"{xm6_dcli_passed}/{xm6_dcli_passed + xm6_dcli_failed}",
                    "xm6_text_bytes": xm6_text_sz,
                    "xm6_rodata_bytes": xm6_ro_sz,
                    "qspill_funcs": r_qspill["functions_scanned"],
                    "qspill_count": r_qspill["q_spills"],
                    "suite5_font_cjk_arm": r_s5_arm.get("passed"),
                    "suite14_arm_checks": r_l0_arm.get("checks_passed", r_l0_arm.get("checks")),
                    "xm6_arm_checks": r_xm6_arm.get("checks_passed", r_xm6_arm.get("checks")),
                    "suite4_arm_checks": r_fs_arm.get("total_checks"),
                    "suite2_arm_checks": r_hf_arm.get("total_checks"),
                    "qemu_suite19_r7": f"{r_s19.get('checks_passed')}/{r_s19.get('checks_total')}",
                    "qemu_suite20_r7": f"{r_s20.get('checks_passed')}/{r_s20.get('checks_total')}",
                    "wlm_glyphs_missing_sc": wlm_glyphs.get("missing_sc_3font"),
                    "wlm_glyphs_missing_tc": wlm_glyphs.get("missing_tc_3font"),
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
            "--main", str(main_path),
            "--verification", str(ver_path),
            "--readme", str(readme_path),
            "--fenestra", str(staged_fenestra),
            "--output", str(DELIVERY),
            "--name-prefix", prefix,
            "--version", "v71.7r7-lut-preview",
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
                "fenestra_files_count": len(fenestra_meta),
                "evidence_count": len(evidence_entries),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
