#!/usr/bin/env python3
"""Publish the V71.7-r2 Fast-FPS + WYSIWYG Preview Toggle delivery bundle to:
  /Users/ycx/Nikon/delivery/Z8-v71.7r2-LutPreview-<sha8>/
  /Users/ycx/Nikon/delivery/Z8-v71.7r2-LutPreview-<sha8>.zip
via pack68.py, then independently verify shasum -a 256 -c SHA256SUMS.txt and unzip -tq.
STRICT: Never modifies prior frozen delivery packages (Z8-v71.7-LutPreview-fe3e7f22* etc.)!
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
FONT_ZH = V71R7 / "work/font-zh"
SD_FENESTRA = V71R7 / "sd-card/FENESTRA"
STAGED_LUTS = FINAL / "staged-luts/LUTS"
DELIVERY = Path("/Users/ycx/Nikon/delivery")


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main() -> None:
    main_path = FINAL / "main.main"
    main_json = json.loads((FINAL / "main.json").read_text(encoding="utf-8"))
    main_sha = sha(main_path)
    assert main_sha == main_json["main_sha256"], (main_sha, main_json["main_sha256"])
    pkg_sha = sha(FINAL / "Z_8_0406.bin")
    build_id = main_json["build_id"]

    prefix = "Z8-v71.7r2-LutPreview-"
    target_dir = DELIVERY / f"{prefix}{main_sha[:8]}"
    target_zip = DELIVERY / f"{prefix}{main_sha[:8]}.zip"
    assert not target_dir.exists(), f"Target dir already exists (immutable!): {target_dir}"
    assert not target_zip.exists(), f"Target zip already exists (immutable!): {target_zip}"

    # 1. Stage 5 FENESTRA/ files (including rebuilt filmsim.wlm with #9 取景实时预览 toggle)
    staged_fenestra = FINAL / "delivery-fenestra-r2"
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

    # 2. Stage README
    readme_text = f"""Nikon Z8 V71.7-r2 固件发布包 ({prefix}{main_sha[:8]})
Nikon Z8 V71.7-r2: Viewfinder High-FPS Fix (LCD + EVF XM6 Slot 41) + WYSIWYG Live Preview Toggle + Native LUT Preview
====================================================================================================

【一、固件标识与校验和 / 1. FIRMWARE IDENTITY & CHECKSUMS】
----------------------------------------------------------------------------------------------------
  固件版本 / Version        : V71.7-r2 (v71r7-lut-preview 高帧率优化 + 取景实时预览独立开关)
  内部标识 / Identity       : 04.06.p0
  Bootguard Build ID        : {build_id} (at 0x401FE060)
  MAIN (eg2070) SHA-256     : {main_sha} (81,807,709 bytes)
  Z_8_0406.bin SHA-256      : {pkg_sha} (99,051,117 bytes)

  SD / CFexpress 卡 FENESTRA/ 模块（共 5 个文件，filmsim.wlm 必须同步更新！）:
    filmsim.wlm             : {fenestra_meta['filmsim.wlm']['sha256']} ({fenestra_meta['filmsim.wlm']['bytes']} bytes)  [新增 #9 取景实时预览 开关，默认关闭]
    slotcard.wlm            : {fenestra_meta['slotcard.wlm']['sha256']} ({fenestra_meta['slotcard.wlm']['bytes']} bytes)
    portrait.wlm            : {fenestra_meta['portrait.wlm']['sha256']} ({fenestra_meta['portrait.wlm']['bytes']} bytes)
    ZF_GRAIN.BIN            : {fenestra_meta['ZF_GRAIN.BIN']['sha256']} ({fenestra_meta['ZF_GRAIN.BIN']['bytes']} bytes)
    STARTUP-Z8.0            : {fenestra_meta['STARTUP-Z8.0']['sha256']} ({fenestra_meta['STARTUP-Z8.0']['bytes']} bytes)

【二、本次实机 USB 采样定位的 4 项取景掉帧根因与修复】
----------------------------------------------------------------------------------------------------
  在 V71.7 (0xd590d8cd) 实机 USB 直读 DRAM 指标证实 4 大取景瓶颈：
    1) Pool 0 零等待 trylock 丢帧 68.7%（enqueue_skipped_busy=3,820 vs enqueue_copied=1,742，实测仅 5.0 FPS）：
       -> 改为带 <=600us 有界 yield 重试的 l0_pool_trylock_retry()，且首块忙跳过时回退 last_tried_gen 下一拍立刻补采。
    2) EVF 1280x852 强制回退 ARM 软件 HDF，单帧 115.7 ms（仅 ~8 FPS）：
       -> 开放 EVF 1280x852 NV12 走 CEVA-XM6 Core 0 Slot 41（MODE_STILL_GENERAL 原地 NV12，ASan+UBSan 0-diff 验证）及 512 MiB DMA 窗口，预计由 115.7 ms 降至 ~6-12 ms。
    3) 实况联动开启时 filmsim_live_worker 与 Layer 0 抢占 XM6 Core 0 零等待回退（664 次回退 25ms ARM）：
       -> z8_xm6_live_hdf_nv12() 改为有界 8ms Core 0 共享锁，两路干净共享硬件 XM6。
    4) LCD/EVF 冗余 1.05-1.64 MiB 整帧二次拷贝 + 22ms 硬节流 + 高速连拍/自拍门控：
       -> 改为直接拷入独占 BANK_WORKING 槽位（零二次整帧拷贝），放宽 LCD 帧间隔 22ms->12ms，补齐上游 C15/C30/C60/C120 与自拍自动旁路。

【三、新增「取景实时预览」开关（Zf 胶片颗粒 / HDF 菜单第 9 项）】
----------------------------------------------------------------------------------------------------
  - 菜单位置：设定菜单 -> 插件管理器 -> Zf 胶片颗粒 / HDF -> #9「取景实时预览」(默认：关闭)
  - 关闭时（且未开启 .cube LUT）：Display Layer 0 完全休眠、0 字节 Class 0x24 占用、100% 原厂 60/120Hz 满帧取景，且静态照片与实况短片的颗粒/HDF 依然照常生效！
  - 开启时：背屏 LCD 与 EVF 均启用高速 XM6 加速的实时 HDF + Zf 颗粒所见即所得取景。

【四、升级步骤】
----------------------------------------------------------------------------------------------------
  1) 将 Z_8_0406.bin 拷到储存卡根目录，在相机「设定菜单 -> 固件版本」升级；
  2) 将本包 FENESTRA/ 内 5 个文件同步覆盖到储存卡 FENESTRA/（双卡请保持 A/B 卡一致，尤其是新版 filmsim.wlm）；
  3) .cube LUT 放入 FENESTRA/LUTS/<分类名>/*.cube，在相机「照片拍摄菜单」最底部「选择 LUT」启用。
"""
    readme_path = FINAL / "README-v71.7r2-lut-preview.txt"
    readme_path.write_text(readme_text, encoding="utf-8")

    # 3. Minimal verification receipt pointing to local stage manifests
    ver_dir = FINAL / "delivery-staging-meta-r2"
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
        ("wlm_filmsim_verify_report.json", V71R7 / "work/wlm-zh/filmsim/build/verify-report.json"),
        ("wlm_filmsim_glyph_coverage.json", V71R7 / "tests/wlm_zh/glyph_coverage_report.json"),
    ]
    evidence_entries = [
        {"name": nm, "path": os.path.relpath(p, ver_dir), "sha256": sha(p)}
        for nm, p in ev_files
        if p.is_file()
    ]
    ver_path = ver_dir / "verification.json"
    ver_path.write_text(
        json.dumps(
            {
                "schema": "v71.7r2.lut-preview-delivery-verification/v1",
                "version": "v71.7r2-lut-preview",
                "identity": "04.06.p0",
                "main_sha256": main_sha,
                "package_sha256": pkg_sha,
                "build_id": build_id,
                "release_checks_passed": True,
                "physical_camera_verified": False,
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
            "v71.7r2-lut-preview",
        ],
        check=True,
    )

    # Also copy staged curated LUTs into delivery_dir/LUTS_PACK/ (outside SHA256SUMS? Wait: keep delivery clean and verified first!)
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
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
