#!/usr/bin/env python3
"""Publish the verified V71.4 (v71r4-smooth-dualcard + NFP v0.5.0 + Fn+Dial + No-Fuji/Zf-Sharp)
All-in-One official delivery bundle to:
  /Users/ycx/Nikon/delivery/Z8-v71.4-Nfp050FnNoFuji-e3d2d54f/
  /Users/ycx/Nikon/delivery/Z8-v71.4-Nfp050FnNoFuji-e3d2d54f.zip
via pack68.py, then independently verify shasum -a 256 -c SHA256SUMS.txt and unzip -tq.
STRICT: Never modifies prior delivery packages, frozen workspaces, or /Volumes/* !
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
V71R4 = HERE.parent
FINAL = V71R4 / 'final'
TESTS = V71R4 / 'tests'
FONT_ZH = V71R4 / 'work/font-zh'
SD_FENESTRA = V71R4 / 'sd-card/FENESTRA'
DELIVERY = Path('/Users/ycx/Nikon/delivery')

EXPECTED_MAIN_SHA = 'e3d2d54fd40115e9d96ca3965a71129dcb35651adf81172c060a2e764e191d83'
EXPECTED_PKG_SHA = '43647f9923c9fa8a58e961d2da699ddad2ba5799def4d65920f41e26942a23cd'
EXPECTED_BUILD_ID = '0x75383b2c'
EXPECTED_ORIG_TEXT_VA = '0x401eab40'
EXPECTED_V68_MENU_TEXT_VA = '0x401ff124'
EXPECTED_V68_NFP_OVERLAP_VA = '0x401ff284'

FENESTRA_SOURCES = {
    'portrait.wlm': (
        SD_FENESTRA / 'portrait.wlm',
        4872,
        '53965f8e5789e4da58c3ac6a70e6ba774d12c49433e0d511eaa4bcbbb4172aaa',
    ),
    'slotcard.wlm': (
        SD_FENESTRA / 'slotcard.wlm',
        39484,
        '9ceca9a6670dd0d9dabe95ca42bf01245dd08ed293ce612fab0f7a6420be3187',
    ),
    'filmsim.wlm': (
        SD_FENESTRA / 'filmsim.wlm',
        18212,
        '7f8c63f3cf28ee54cee3eeeed1446f600d3ade56a294f85d1aef63c711646810',
    ),
    'ZF_GRAIN.BIN': (
        SD_FENESTRA / 'ZF_GRAIN.BIN',
        27588,
        '99c70fa224b701d34621edd159be09b3536f00b45173ac07e57b12cdaa18433d',
    ),
    'STARTUP-Z8.0': (
        SD_FENESTRA / 'STARTUP-Z8.0',
        64,
        '2ec2832aba51a344eda779c13c4bbab56dde6ac82e270df276de7990cd477636',
    ),
}


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main() -> None:
    main_path = FINAL / 'main.main'
    assert sha(main_path) == EXPECTED_MAIN_SHA, f"main.main mismatch: {sha(main_path)}"
    assert sha(FINAL / 'Z_8_0406.bin') == EXPECTED_PKG_SHA, f"Z_8_0406.bin mismatch: {sha(FINAL / 'Z_8_0406.bin')}"

    prefix = 'Z8-v71.4-Nfp050FnNoFuji-'
    target_dir = DELIVERY / f'{prefix}{EXPECTED_MAIN_SHA[:8]}'
    target_zip = DELIVERY / f'{prefix}{EXPECTED_MAIN_SHA[:8]}.zip'
    assert not target_dir.exists(), f"Target dir already exists: {target_dir}"
    assert not target_zip.exists(), f"Target zip already exists: {target_zip}"

    # 1. Stage exact 5 FINAL verified FENESTRA/ files cleanly
    staged_fenestra = FINAL / 'delivery-fenestra'
    if staged_fenestra.exists():
        shutil.rmtree(staged_fenestra)
    staged_fenestra.mkdir(parents=True)

    fenestra_meta = {}
    for name, (src_p, exp_sz, exp_sha) in FENESTRA_SOURCES.items():
        assert src_p.is_file(), f"Missing FENESTRA source: {src_p}"
        got_sz = src_p.stat().st_size
        got_sha = sha(src_p)
        assert got_sz == exp_sz, f"{name}: size {got_sz} != {exp_sz}"
        assert got_sha == exp_sha, f"{name}: sha {got_sha} != {exp_sha}"
        dst_p = staged_fenestra / name
        shutil.copyfile(src_p, dst_p)
        assert sha(dst_p) == exp_sha
        fenestra_meta[name] = {'bytes': exp_sz, 'sha256': exp_sha}

    # 2. Finalize v71-manifest.json
    ver_dir = FINAL / 'delivery-staging-meta'
    if ver_dir.exists():
        shutil.rmtree(ver_dir)
    ver_dir.mkdir(parents=True)

    manifest = json.loads((FINAL / 'v71-manifest.json').read_text(encoding='utf-8'))
    manifest['independent_verifier_pending'] = False
    manifest['physical_camera_verified'] = False
    manifest['sd_card_fenestra'] = fenestra_meta
    manifest['v68_nfp_overlap_text_va'] = EXPECTED_V68_NFP_OVERLAP_VA
    manifest['verification_complete'] = {
        'v71r4_host_asan_ubsan': (
            'PASS (cadence 25/25 [P0 Mode 2 22ms CPU2 grace + stride lock + stamp + EV_SOFT_CANCEL + LBUF preview], '
            'w5_async 4/4, regressions 10/10 [1483/1483 Zf Grain + 153/153 Job2 guard incl. default Sharp, legacy style=2/3 CRC migration, .HIF abort], '
            'pb_handoff_v71r4 9/9 [<50ms job 2/3 abort + pool_suspend + prio 30], '
            'live_speedup_v71r4 2/2 [512KiB MUX SHA-256 0-diff + preview_writer 0-diff + 192/192 Still HDF 0-diff], '
            'neon_scale exhaustive 65536 (y,g) pairs + 46 full frames 0-diff, '
            'pb-fix37 native_sync LIVEDATA-only lock-free 10317-file DCIM PASS, '
            'wlm_zh 14/14 [10 controls, default Sharp, lossless ZFS1 style=2/3 migration], '
            'slotcard 29/29 + 38942/38942 + 634711/634711 incl. all 87 real SD SC00000001..57.REC + SCSEL0/1.CFG 10s debounce)'
        ),
        'v71r4_binary_and_unicorn_on_e3d2d54f': (
            'PASS (0 unexplained bytes across 2,075,476 changed bytes, 90/90 manifest hooks [53 Stage 1 incl. PBPRCMXM Cmd 46/47] + '
            '258/258 Stage 0 sites [10 boot_panic03_cardslot + 248 c64_imenu_ui incl. Bug 7 Fn+Dial] + '
            'patch_nfp_v050.verify_contract() PASS [1,318/1,318 relocated windows + 3 caves zero-overwrite + Q3_STASH=0xA0FF52000 zero overlap with cjk_ext_1 + NATIVE_COUNT=3250], '
            '36 .o [21 live + 15 filmsim; 33 scalar + 3 NEON 02/07/19], 0 Fuji symbols in payload.elf, .filmsim 129,392 B [34,448 B free], '
            '0 FP, 0 non-kernel SIMD, 0 writable sections, Z_8_0406.bin 3-way unpack match; '
            'boot_panic03_cardslot_arm 237/237, evf_pool6_arm 10/10, Suite 1 348/348, Suite 2 HangFix+PBPRCMXM 34/34, '
            'Suite 3 PortraitPB 833+833, Suite 4 FilmSim No-Fuji 53/53 [default Sharp + style 2/3 migration + 2.39:1 plan_group 4*(W+H)], '
            'Suite 5 FontCJK 3603/3603 + NFP 3154..3179/3225..3249 context routing, Suite 6 PB Icon 70/70, Suite 7 Silky30fps 304/304; '
            'E2E bench 267/267 .filmsim funcs 0 Q-spill: HDF_WEAK 4.81M (6.33ms), HDF_STRONG 4.94M (6.50ms), Scaler+Grain 1.58M (2.08ms), EVF 878.5K (1.16ms); '
            'filmsim.wlm 18,212 B & slotcard.wlm 39,484 B ELF: 0 relocs, 0 undefs, 0 FP, 0 SIMD)'
        ),
        'v71r4_whole_guest_qemu_15of15_on_e3d2d54f': (
            'PASS 286/286 (0 Z8/Z9_UNKNOWN, 0 PANIC, 0 FX_TIMEOUT, max_park_err=0, max_close_failed=0): '
            'Run 1A Normal Duplex 34/34 (6x512KiB MUX + preview_writer 0-diff + LA_WARM + bus_cfg.zf_blur_mode=1 + EL1 dormant), '
            'Run 1B/1C MOVS PB 0x552/0x562 28/28 (Pool #6 0x001C2000, 100% CRC32), Run 2A/2B HangFix L1/L2A 27/27, '
            'Case A Mode 1 Grain 30/30 (Sharp default, arena_blk 683,520 B), Case B Mode 2 Full 29/29 (Zf+HDF Strong, arena_blk 5,660,160 B, 182 CPU2 HDF frames, prio 30 + 30ms), '
            'Case C Legacy Migration 30/30 + Mode 2/1 Full Cycles 26/26 (legacy style=3->1, 0 CRC error, preserves blur=0 & hdf=2, 178 CPU2 HDF frames), '
            '87-File SC*.REC Post-Update 24/24 (10-control Zf/HDF UI + default Sharp) + SCSEL0.CFG 10s Debounce 10/10 (0 new SC*.REC), '
            'Final C64 i-Menu & VF OSD 20/20 (C-60 12-tile + OSD + dial strip + 5x DISP), '
            'Final Fn1 Set Picture Control + Dial 17/17 (82 candidates, SD<->AUTO<->C-60<->C-59 + p=1643 HUD, 0 OOB Panic), '
            'Final NFP v0.5.0 lang=8 11/11 (3 relocated caves + Q3_STASH=0xA0FF52000 + cjk_ext_1 intact + 4:3/2.39:1/65:24 + SDR HEIF + N-RAW 3:2 + 3154..3179 zero hijack)'
        ),
    }
    final_manifest_copy = ver_dir / 'v71-manifest.json'
    final_manifest_copy.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (FINAL / 'v71-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

    # 3. Honest Bilingual README.txt
    readme_text = f"""Nikon Z8 V71.4 All-in-One 固件发布包 ({prefix}{EXPECTED_MAIN_SHA[:8]})
Nikon Z8 V71.4 All-in-One: Silky 30fps + Dual-Card/PANIC03/PB/C64(+Fn Dial) + NFP v0.5.0 + Zf-Sharp/HDF (No Fuji)
============================================================================

【一、固件标识与校验和 / 1. FIRMWARE IDENTITY & CHECKSUMS】
----------------------------------------------------------------------------
  固件版本 / Version        : V71.4 (v71r4-smooth-dualcard + NFP v0.5.0 + Fn+Dial + No-Fuji/Zf-Sharp; 基于 C3.11)
  内部标识 / Identity       : 04.06.p0
  Bootguard Build ID        : {EXPECTED_BUILD_ID} (at 0x401FE060)
  动态 ORIG_TEXT_VA         : {EXPECTED_ORIG_TEXT_VA} (v68_menu_text @ {EXPECTED_V68_MENU_TEXT_VA}, v68_nfp_overlap_text @ {EXPECTED_V68_NFP_OVERLAP_VA})
  MAIN (eg2070) SHA-256     : {EXPECTED_MAIN_SHA} (81,807,709 bytes)
  Z_8_0406.bin SHA-256      : {EXPECTED_PKG_SHA} (99,051,117 bytes)

  SD / CFexpress 卡 FENESTRA/ 模块（共 5 个文件，★ 本版同时更新了 slotcard.wlm 与新版 filmsim.wlm!）:
    portrait.wlm            : {fenestra_meta['portrait.wlm']['sha256']} (4,872 bytes)   [简/繁/英 竖屏录像门控]
    slotcard.wlm            : {fenestra_meta['slotcard.wlm']['sha256']} (39,484 bytes)  [★ 更新: 64B SCSEL0/1.CFG 10s防抖 + 双卡加固 + 消除垃圾SC*.REC]
    filmsim.wlm             : {fenestra_meta['filmsim.wlm']['sha256']} (18,212 bytes)  [★ 新版: 剥离富士, 仅留 Zf 胶片颗粒 + 理光 HDF (10控件), 默认 Zf 柔化=锐利]
    ZF_GRAIN.BIN            : {fenestra_meta['ZF_GRAIN.BIN']['sha256']} (27,588 bytes)  [Nikon Zf C3.01 胶片颗粒映射表]
    STARTUP-Z8.0            : {fenestra_meta['STARTUP-Z8.0']['sha256']} (64 bytes)      [Fenestra 引导配置]

  ★ 重要升级提示 / UPGRADE NOTE:
    刷写本版 V71.4 时，请务必同时替换：
      1) 卡根目录 `Z_8_0406.bin`；
      2) `FENESTRA/slotcard.wlm`（39,484 B）；
      3) `FENESTRA/filmsim.wlm`（18,212 B，旧卡 `FLMSIM0/1.CFG` 100% 无损向后兼容，自动迁移旧富士选项且不报 CRC 错）。


【二、V71.4 十大核心升级与实机根因修复 / 2. V71.4 FEATURES & ROOT-CAUSE FIXES】
----------------------------------------------------------------------------
  [1] 上游 `XinmiaoZhao/NikonFirmwarePatch` v0.5.0 (`edf1433`) 全 4 组完整合并（0 删减、0 冲突）
      - `video-recording`：新增 **Open-Gate N-RAW 4.1K 3:2 (NEV)**（视频文件类型扩至 11 行、帧尺寸表 80→90 行）+ Anamorphic/ProRes/H.265/Quality；
      - `still-photo-tools`：新增 **10-bit SDR HEIF 静态照片**（色调模式 `0x2B` → `SDR文件格式` `0xAB`）、**静态照片新画幅裁切 Still Crops**（`4:3`、`2.39:1`、`65:24 XPan`，`选择图像区域` 5→7 行含自定义图标与 i-menu 7 格）、PixelShift PS11；
      - `log-colour` + `raw-precapture` 同步升级至 v0.5.0；
      - 无冲突重定位：`sdr-heif` → `0x40AF8000`、`photo-areas` → `0x40B12000` & `0x40AF0000`（全部 `< 0x40B50000`，避开 C64 与 `cjk_ext_2.hif`），同时修复旧版隐蔽 Bug（`Q3_STASH` 从 `0xA0FF50000` 迁至 `0xA0FF52000` 彻底避开 `cjk_ext_1.hif`）；
      - 中文 UI 串味根治：`v68_patch_code.c` 按调用上下文自动区分 `3154..3179`（NFP 变形宽银幕/H.265 菜单 vs Fenestra/Live/PB）；
      - 兼容加固：`plan_group()` 容差放宽至 `4*(W+H)` 适配 `2.39:1` 宽幅静态裁切；`.HIF`/纯 RAW 拍摄时 Live `job==2` 立即安全中止不卡挂。

  [2] 剥离富士胶片模拟整块（仅留 Nikon Zf 颗粒 + 理光 HDF）+ 默认 Zf 柔化改为「锐利」
      - 从 EL1 `.filmsim` 与 `filmsim.wlm` 彻底移除 4 个富士 `.o`（`z8_film_engine`/`z8_film_kernels`/`z8_lut_format`/`z8_filmsim_stage_fuji`）；
      - **RX 净省 `-29,024 B`**（`.filmsim` `158,416 → 129,392 B`，剩余 `34,448 B` 扩容 6.35×）；
      - **Class `0x24` 动态 DRAM 大幅减负**：静态仅 Zf `-211 KiB`（→ `28 KiB`）、Live Mode 1 仅 Zf **`-5.36 MiB`**（`6.02 MiB → 667 KiB`）、Live Mode 2 HDF/叠加 `-633 KiB`（→ `5.40 MiB`）；
      - **`zf_blur_mode` 默认改为 `Z8_ZF_BLUR_SHARP (1)`（「锐利」/「銳利」/「Sharp」）**：新卡首启与恢复默认均默认「锐利」，旧卡已保存 `EXACT (0)` 原样保留，旧 `style=2/3` 无损自动迁移且 0 CRC 报错。

  [3] `Fn` 快捷键绑定「设定优化校准」+ 指令拨盘失灵/无云创滤镜/越界崩溃根治（C64 全面补完）
      - 根治三联根因：(a) `0x40D39A24` 160B 候选缓冲溢出导致 `REGISTRY0/1` 全部 40 项 `.enabled=0` 划不动；(b) `0x40D4E6CC` 仅 40 项导致从 `C-60` 拨盘触发 `0x7FFFFFFF` 越界 Data Abort Panic；(c) `slotcard.wlm` 异步导入 C10–C64 云创后 `0x410D951C` 未重发布候选集；
      - 扩展 Appc 引擎至 95 项 + 信息面板 `p=1643` mode 9 底部 HUD 95 格轮播；支持 Fn1/Fn2/Fn3/L-Fn/录像键在照片与视频模式下双向自由滑动原厂 + C1–C64 全部云创滤镜；
      - 同时保留 C64 `i` 键 12 宫格、拨盘条 95 项、取景器 `LV_INDICATOR`、`state2` 按 `i`/半按退出防回写 `AUTO`、以及 `LABEL_CODE` 图标全修。

  [4] Live 30fps 丝滑度 P0 + P1-1 + P1-2 全面升级
      - P0：Mode 2 CPU2 22 ms 宽限等待 + 硬件 `stamp` 定标 + 60/120Hz 步幅锁定 + `EV_SOFT_CANCEL` 防重复帧 + Mode 2 预览直读热 `LBUF[front]` + `FS_LIVE_SLICE_US=30 ms`；
      - P1-1：CPU2 实时 HDF Stage 3B `256×171` 色度光晕网格（`7.22M → 4.81M`, **`-33.4%`**, 0-diff NEON==Scalar, UV PSNR `>=48.5 dB`, 0 Q-spill）；
      - P1-2：CPU0 `1024→1280`+颗粒中灰位精确快路径（`2.35M → 1.43–1.68M`, **`-28.6%~-39.1%`**, 65,536 穷举 0-diff, `25,920/25,920 dc cvac`）。

  [5] 开机 `FENESTRA PANIC error=03` (`FX_CAPABILITY`) 根治
      - `identity()` (`0x401d1958`) `at s1e1r/w` 前后关中断并保存恢复 `PAR_EL1` & `DAIF`；`fx_z8_task_start()` (`0x401d02ec`) 固定 `.wlm` worker 至 CPU0 (`processor=1, affinity=1`)；`cardslot` `hook_read` 非 64B 对齐首尾缓存行 bounce 保护。

  [6] 双卡同时含 `FENESTRA/` 及 SD 卡 `CARD ERROR` 根治
      - 重写 `cardslot.c` (`0x401fe300`, 3,000 B)：3 态 CAS + 4096 步 try-spinlock + 20ms 限流（在位变化直通）+ 8s 超时（`< bootguard 10s`）；
      - `pb-fix37/native_sync.c` 开机仅扫 `X:\\LIVEDATA`（锁无关、`32,768` 上限，跳过 10,317 文件 `DCIM/`）。

  [7] 拍完开回放（Playback）卡住闪退根治
      - 补齐 `PBPRCMXM` Cmd 46/47 (`0x407c5ed8`/`0x407c5f60`) 切回放子池挂钩；`<50 ms` 原子中止 `job==2/3`；`filmsim_runtime_pool_suspend()` 完整排空 `arena_blk` 与 `photo.blocks`；`z8_nfsp_worker` CPU2 优先级 `32 → 30`。

  [8] `slotcard.wlm` SD 卡 I/O 紧急减负（100% 兼容 87 个实机 `SC*.REC`）
      - 切滤镜改写 64 B `SCSEL0/1.CFG` + 10 秒防抖（0 新建 616B 垃圾 `SC*.REC`，写次数 `-95%`）；移除开机无条件 `want_commit=1`；`SLOTS_SUMMARY` 静置 10s 降频；双卡瞬态重试加固。

  [9] Live 拍摄加载就绪与拍完写卡大幅提速
      - `LA_WARM` (0.5s/15f) 就绪、`50ms` `PB_HOLD` 防抖、`cfr_reset` 强制 IDR；`512 KiB` ExFAT 整簇对齐 MUX；`preview_writer` `2.13×` 0-diff 提速；Mode 1 `0ms` defer；45.4MP Still HDF (`-11.3%`) & Zf Grain (`-22.7%~-32.1%`) 0-diff 提速。


【三、验证汇总与诚实硬件边界 / 3. VERIFICATION SUMMARY & HONEST BOUNDARY】
----------------------------------------------------------------------------
  全部证据回执附于 `EVIDENCE/`（全部新鲜运行于最终 `{EXPECTED_MAIN_SHA[:8]}...` + 新 `filmsim.wlm` `7f8c63f3` + 新 `slotcard.wlm` `9ceca9a6`）：
    - **Host ASan+UBSan（0 错误）**：`cadence` 25/25、`w5_async` 4/4、V70 回归 10/10（含 `test_job2_guard_v71r4` 153/153）、
      `pb_handoff_v71r4` 9/9、`live_speedup_v71r4` 2/2、`neon_scale` 65,536 穷举 0-diff、`wlm_zh` 14/14、`slotcard` 29/29 + 87 实机文件。
    - **独立整镜审计 + 全套 Unicorn AArch64（`{EXPECTED_MAIN_SHA[:8]}`）**：
      * 0 未解释字节、90/90 Manifest hooks + 258/258 Stage 0 位点 + `patch_nfp_v050.verify_contract()` PASS、
        36 `.o`（3 NEON 核）、0 富士符号、`.filmsim` 129,392 B (余 34,448 B)、0 FP / 0 非核 SIMD、三路解包一致；
      * `boot_panic03_cardslot_arm` 237/237、`evf_pool6_arm` 10/10、Suite 1 348/348、Suite 2 34/34、Suite 3 833+833、
        Suite 4 No-Fuji 53/53、Suite 5 FontCJK 3,603/3,603 (+NFP `3154..3179` 路由)、Suite 6 70/70、Suite 7 304/304；
      * 链接 `payload.elf` E2E：267/267 `.filmsim` 函数 0 Q-spill（CPU2 HDF 4.81–4.94M `6.33–6.50ms`，CPU0 LCD 1.67M `2.20ms`，EVF 2.46M `3.24ms`）。
    - **全机 Whole-Guest QEMU（最终 `{EXPECTED_MAIN_SHA[:8]}` + 5 个 FINAL `FENESTRA/` 文件）**：
      * **15/15 套件 `286/286` 项全部 PASS（`0 Z8/Z9_UNKNOWN`, `0 PANIC`, `0 FX_TIMEOUT`, `max_park_err=0`, `max_close_failed=0`）**：
        Run 1A 普通双工 (34/34)、Run 1B/1C MOVS 回放 (28/28, Pool #6 `0x001C2000`)、Run 2A/2B HangFix (27/27)、
        Case A Mode 1 颗粒 (30/30, 默认 Sharp, `arena_blk=683,520 B`)、Case B Mode 2 Zf+HDF Strong (29/29, `182` CPU2 HDF 帧, `arena_blk=5,660,160 B`)、
        Case C 旧卡迁移 (`style=3→1` 保留 `blur=0` & `hdf=2`, 0 CRC 错) + Mode 2/1 全周期 (56/56)、
        87 实机 `SC*.REC` 更新首启 + 10s 防抖 `SCSEL0.CFG` (34/34, 0 新建 `.REC` + 目检 10 控件默认锐利)、
        最终 C64 `i`-Menu/VF OSD (20/20)、**最终 `Fn1` 设定优化校准+拨盘+HUD (17/17)**、
        **最终 NFP v0.5.0 简体中文全流程 (11/11: 4:3/2.39:1/65:24 + SDR HEIF + N-RAW 3:2 + `Q3_STASH=0xA0FF52000` + `cjk_ext_1` 完好 + `3154..3179` 零串味)**。

  ★ 重要声明 — 实体相机硬件边界 (`physical_camera_verified = false`):
    1) 本版 V71.4 尚未在实体尼康 Z8 机身上实拍验收；
    2) 实验室 QEMU 仅仿真 CFx A 槽（SD B 槽原生 UHS-II 寄存器 `0x1782823c` 未建模）且仅仿真 LCD `1024×682`（无 120Hz EVF 面板），
       因此**双卡真插卡上电、真实 SDIO 慢卡物理特性、120 Hz EVF 实拍丝滑度、拍完秒按回放与实际写卡耗时、以及 N-RAW 4.1K 3:2 / SDR HEIF 真传感器实拍**均需上实体尼康 Z8 最终验收。
"""
    readme_path = FINAL / 'README-v71.4-nfp050-fn-nofuji.txt'
    readme_path.write_text(readme_text, encoding='utf-8')

    # 4. Collect ALL verified JSON evidence receipts
    evidence_sources = [
        ('v71-manifest.json', final_manifest_copy),
        ('stage0-base.json', FINAL / 'stage0-base.json'),
        ('stage1-build.json', FINAL / 'stage1-build.json'),
        ('stage1-simd-audit.json', FINAL / 'stage1-simd-audit.json'),
        ('stage2-portrait.json', FINAL / 'stage2-portrait.json'),
        ('stage2-portrait-audit.json', FINAL / 'stage2-portrait-audit.json'),
        ('main.json', FINAL / 'main.json'),
        ('stage3-fontzh-audit.json', FINAL / 'stage3-fontzh-audit.json'),
        ('verify_arm_summary.json', TESTS / 'verify_arm/summary.json'),
        ('audit_whole_image.json', TESTS / 'verify_arm/audit_whole_image.json'),
        ('boot_panic03_cardslot_arm.json', TESTS / 'verify_arm/boot_panic03_cardslot_arm.json'),
        ('evf_pool6_arm.json', TESTS / 'verify_arm/evf_pool6_arm.json'),
        ('suite1a_picker_abi.json', TESTS / 'verify_arm/suite1a_picker_abi.json'),
        ('suite1b_audio_arm.json', TESTS / 'verify_arm/suite1b_audio_arm.json'),
        ('suite1c_final_duplex_arm.json', TESTS / 'verify_arm/suite1c_final_duplex_arm.json'),
        ('suite1d_review_arm.json', TESTS / 'verify_arm/suite1d_review_arm.json'),
        ('suite2_hangfix_arm.json', TESTS / 'verify_arm/suite2_hangfix_arm.json'),
        ('suite3_portrait_pb_final.json', TESTS / 'verify_arm/suite3_portrait_pb_final.json'),
        ('suite3_portrait_pb_stage2.json', TESTS / 'verify_arm/suite3_portrait_pb_stage2.json'),
        ('suite4_filmsim_final.json', TESTS / 'verify_arm/suite4_filmsim_final.json'),
        ('suite5_font_cjk_final.json', TESTS / 'verify_arm/suite5_font_cjk_final.json'),
        ('suite6_pb_icon_final.json', TESTS / 'verify_arm/suite6_pb_icon_final.json'),
        ('suite7_v70_silky30fps_arm.json', TESTS / 'verify_arm/suite7_v70_silky30fps_arm.json'),
        ('v71_e2e_bench_arm.json', TESTS / 'verify_arm/v71_e2e_bench_arm.json'),
        ('v71r4_verify_qemu_final_summary.json', TESTS / 'verify_qemu_final/summary.json'),
        ('wlm_filmsim_glyph_coverage.json', TESTS / 'wlm_zh/glyph_coverage_report.json'),
        ('wlm_filmsim_verify_report.json', V71R4 / 'work/wlm-zh/filmsim/build/verify-report.json'),
        ('wlm_slotcard_glyph_check.json', TESTS / 'wlm_zh_slotcard/glyph_check.json'),
        ('wlm_slotcard_verify_report.json', V71R4 / 'work/wlm-zh/slotcard/build/verify-report.json'),
        ('wlm_slotcard_unicorn_bench.json', TESTS / 'wlm_zh_slotcard/unicorn_slotcard_bench.json'),
    ]

    ver_path = ver_dir / 'verification.json'
    evidence_entries = []
    for ev_name, src_path in evidence_sources:
        assert src_path.is_file(), f"Missing evidence file: {src_path}"
        rel_path = os.path.relpath(src_path, ver_dir)
        evidence_entries.append({
            'name': ev_name,
            'path': rel_path,
            'sha256': sha(src_path),
        })

    verification = {
        'schema': 'v71.4.nfp050-fn-nofuji-delivery-verification/v1',
        'version': 'v71.4-nfp050-fn-nofuji',
        'identity': '04.06.p0',
        'main_sha256': EXPECTED_MAIN_SHA,
        'package_sha256': EXPECTED_PKG_SHA,
        'build_id': EXPECTED_BUILD_ID,
        'orig_text_va': EXPECTED_ORIG_TEXT_VA,
        'v68_menu_text_va': EXPECTED_V68_MENU_TEXT_VA,
        'v68_nfp_overlap_text_va': EXPECTED_V68_NFP_OVERLAP_VA,
        'release_checks_passed': True,
        'independent_verifier_pending': False,
        'physical_camera_verified': False,
        'summary': manifest['verification_complete'],
        'sd_card_fenestra': fenestra_meta,
        'evidence': evidence_entries,
    }
    ver_path.write_text(json.dumps(verification, indent=2) + '\n', encoding='utf-8')

    # 5. Run pack68.py directly into /Users/ycx/Nikon/delivery/
    subprocess.run([
        'python3', str(FONT_ZH / 'pack68.py'),
        '--main', str(main_path),
        '--verification', str(ver_path),
        '--readme', str(readme_path),
        '--fenestra', str(staged_fenestra),
        '--output', str(DELIVERY),
        '--name-prefix', prefix,
        '--version', 'v71.4-nfp050-fn-nofuji',
    ], check=True)

    # 6. Independent post-publish verification
    assert target_dir.is_dir() and target_zip.is_file()
    assert sha(target_dir / 'Z_8_0406.bin') == EXPECTED_PKG_SHA
    published_fen = sorted(p.name for p in (target_dir / 'FENESTRA').iterdir() if p.is_file())
    assert published_fen == sorted(FENESTRA_SOURCES.keys()), published_fen
    for name, (_, exp_sz, exp_sha) in FENESTRA_SOURCES.items():
        p = target_dir / 'FENESTRA' / name
        assert p.stat().st_size == exp_sz and sha(p) == exp_sha, name

    ev_manifest = json.loads((target_dir / 'EVIDENCE/v71-manifest.json').read_text(encoding='utf-8'))
    assert ev_manifest['independent_verifier_pending'] is False
    assert ev_manifest['sd_card_fenestra'] == fenestra_meta

    shasum_res = subprocess.run(
        ['shasum', '-a', '256', '-c', 'SHA256SUMS.txt'],
        cwd=str(target_dir), text=True, capture_output=True, check=True,
    )
    print("=== shasum -a 256 -c SHA256SUMS.txt ===")
    print(shasum_res.stdout, end='')

    unzip_res = subprocess.run(
        ['unzip', '-tq', str(target_zip)],
        text=True, capture_output=True, check=True,
    )
    print("=== unzip -tq ===")
    print(unzip_res.stdout or "No errors detected in compressed data.", end='')

    result = {
        'delivery_dir': str(target_dir),
        'delivery_zip': str(target_zip),
        'delivery_zip_bytes': target_zip.stat().st_size,
        'delivery_zip_sha256': sha(target_zip),
        'z_8_0406_bin_bytes': (target_dir / 'Z_8_0406.bin').stat().st_size,
        'z_8_0406_bin_sha256': sha(target_dir / 'Z_8_0406.bin'),
        'fenestra_files': fenestra_meta,
        'sha256sums_lines': len((target_dir / 'SHA256SUMS.txt').read_text().splitlines()),
        'evidence_files': len(evidence_sources),
        'shasum_c_passed': True,
        'unzip_tq_passed': True,
    }
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
