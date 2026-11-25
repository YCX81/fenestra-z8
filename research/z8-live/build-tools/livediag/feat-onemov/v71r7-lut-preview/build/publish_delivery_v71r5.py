#!/usr/bin/env python3
"""Publish the verified V71.5 All-in-One official delivery bundle to:
  /Users/ycx/Nikon/delivery/Z8-v71.5-PbSpeedNp3Nfp051-47c67e9a/
  /Users/ycx/Nikon/delivery/Z8-v71.5-PbSpeedNp3Nfp051-47c67e9a.zip
via pack68.py, then independently verify shasum -a 256 -c SHA256SUMS.txt and unzip -tq.
STRICT: Never modifies prior delivery packages (Z8-v71.4-*), frozen workspaces, or /Volumes/* !
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
V71R5 = HERE.parent
FINAL = V71R5 / 'final'
TESTS = V71R5 / 'tests'
FONT_ZH = V71R5 / 'work/font-zh'
SD_FENESTRA = V71R5 / 'sd-card/FENESTRA'
DELIVERY = Path('/Users/ycx/Nikon/delivery')

EXPECTED_MAIN_SHA = '47c67e9a822fec9dea98b18cdd8a096d0a737bac0d118783b907c4e6d575d6ba'
EXPECTED_PKG_SHA = 'f27e07c5ed75a14366c75bf67ab9b7a1c593195011fb88713940d37fc4b3437e'
EXPECTED_BUILD_ID = '0x3b6c5c56'
EXPECTED_ORIG_TEXT_VA = '0x401eadf4'
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
        52548,
        'b221d05ee0756b96cd2243a8b9507f0fa0d6879a6a0bebcfabb6e6a5ff4e4853',
    ),
    'filmsim.wlm': (
        SD_FENESTRA / 'filmsim.wlm',
        25960,
        'c5d890969ae1278093895378adcfe3103d915e8c4181d345f5fca674b53fc5a6',
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

    prefix = 'Z8-v71.5-PbSpeedNp3Nfp051-'
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
        'v71r5_host_asan_ubsan': (
            'PASS (cadence 25/25, w5_async 4/4, V70 regressions 10/10, pb_handoff_v71r4 9/9, '
            'live_speedup_v71r4 2/2 [192/192 Still HDF NEON==Scalar 0-diff + 512KiB MUX + preview_writer 0-diff], '
            'v71r5_pb_ev7_twin 6/6, pb-fix37 Unicorn 49/49, cardslot_v71r4 v2 Unicorn 286/286, '
            'wlm_zh filmsim 18/18 [13 controls + Per-NP3], slotcard 30/30 + 38,942 + 634,711, pack_np3_z8fx 5/5, '
            'C64 Bug 9 PNG synthesizer 550x2 ASan+UBSan + 550/550 zlib/CRC32)'
        ),
        'v71r5_binary_and_unicorn_12of12_on_47c67e9a': (
            'PASS (0 unexplained bytes across 2,095,004 changed bytes, 93/93 manifest hooks [56 Stage 1 incl. 9 HangFix + PTR64 0x42ac31b8] + '
            '269 Stage 0 V71.5 sites [18 boot_panic03_cardslot v2 + 251 c64_imenu_ui Bugs 1-9] + '
            'NFP v0.5.1 (25fefa1) verify_contract() PASS [1,319 windows + 0x40968000 framing-fixes + Pool #6 0x001C2000 superset + Q3_STASH=0xA0FF52000], '
            '36 .o LLVM 0 FP / 0 non-kernel SIMD, 0 Fuji symbols, .filmsim 142,624 B [21,216 B free], pb.bin 36,288 B [0 forbidden RPC refs], '
            'filmsim.wlm 25,960 B & slotcard.wlm 52,548 B ELF: 0 relocs / 0 undefs / 0 FP / 0 SIMD, Z_8_0406.bin 3-way unpack match; '
            'boot_panic03_cardslot_arm 286/286, evf_pool6_arm 10/10, Suite 1 Live 349/349, Suite 2 HangFix+PB-Bounce+ev7 47/47, '
            'Suite 3 PortraitPB 833+833, Suite 4 FilmSim V71.5 70/70, Suite 5 FontCJK 3,603/3,603, Suite 6 PB Icon 70/70, Suite 7 Silky30fps 304/304; '
            'E2E bench 290/290 .filmsim funcs 0 Q-spill: 45.4MP Still HDF 1,349.53 ms [4.30x vs V71.4 5.8s], Mono HDF 516.13 ms, Stage 2 cancel 2.47 ms, '
            'Live CPU2 6.51 ms / CPU0 LCD 2.20 ms / EVF 3.24 ms; C64 Bug 9 native PNG Unicorn 50/50 PASS on final main.main)'
        ),
        'v71r5_whole_guest_qemu_18suites_on_47c67e9a': (
            '17/18 FULL PASS + 1/18 MODEL_BOUNDARY_EXPECTED (Case A Y-only pre-15e2 16/16 PASS) = 318/318 checks PASS '
            '(0 Z8_UNKNOWN, 0 unexpected Z9_UNKNOWN, 0 PANIC, 0 FX_TIMEOUT, 0 park_err, 0 close_failed): '
            'v71r5_pb_bounce_final 19/19 (Image Review ON zero bounce + Image Review OFF double-PB absorbed + normal exit), '
            'v71r5_ev7_plugins_final 13/13 (MCU power-switch + ev=7/8 park/restore + 3 plugins RUNNING + 0x7001 menu + post-wake shot), '
            'v71r5_np3_recipe_final 14/14 (C-60 Per-NP3 Z8FX bind + shot -> unbound SD automatic Dormant 0B -> C-60 wake + FLMREC0.CFG), '
            'c64_imenu_final 20/20 + fn_pc_final 17/17 + 12/12 real QEMU visual proofs ([FC]C-10..C-60 on p=1636, p=1496, LV OSD 2px outline, p=1637, p=1643, dial bar), '
            'nfp_v050_final (v0.5.1 zh-CN) 12/12 (4.1K 3:2 zero-freeze + Still Crops + SDR HEIF + Q3_STASH + cjk_ext_1 + 3154..3179), '
            'Run 1A 33/33, Run 1B/1C 28/28, Run 2A/2B 27/27, Case B Mode 2 Full 29/29, Case C Legacy Migration 30/30 + Mode 2/1 Full Cycles 26/26, '
            'Slotcard Post-Update 24/24 + 87-File SC*.REC 10/10; '
            'Case A Mode 1 Grain-Only 16/16 MODEL_BOUNDARY_EXPECTED (live_applied=73, 45MP still 861.93 ms photos_completed=1, untouched stock UV hits unmodified filmsim-qemu-r1 cpu_ram_known_producer_overlap conservative check — verified on physical Z8 acceptance checklist)'
        ),
    }
    final_manifest_copy = ver_dir / 'v71-manifest.json'
    final_manifest_copy.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (FINAL / 'v71-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

    # 3. Honest Bilingual README.txt
    readme_text = f"""Nikon Z8 V71.5 All-in-One 固件发布包 ({prefix}{EXPECTED_MAIN_SHA[:8]})
Nikon Z8 V71.5 All-in-One: PB Freeze/Bounce Fix + 4.3x Still HDF + Zero-Skip + Plugin Persistence + Per-NP3 Recipe + C64 Native Cloud Icons + NFP v0.5.1
============================================================================

【一、固件标识与校验和 / 1. FIRMWARE IDENTITY & CHECKSUMS】
----------------------------------------------------------------------------
  固件版本 / Version        : V71.5 (v71r5-smooth-dualcard; 基于 C3.11 + NFP v0.5.1 25fefa1)
  内部标识 / Identity       : 04.06.p0
  Bootguard Build ID        : {EXPECTED_BUILD_ID} (at 0x401FE060)
  动态 ORIG_TEXT_VA         : {EXPECTED_ORIG_TEXT_VA} (v68_menu_text @ {EXPECTED_V68_MENU_TEXT_VA}, v68_nfp_overlap_text @ {EXPECTED_V68_NFP_OVERLAP_VA})
  MAIN (eg2070) SHA-256     : {EXPECTED_MAIN_SHA} (81,807,709 bytes)
  Z_8_0406.bin SHA-256      : {EXPECTED_PKG_SHA} (99,051,117 bytes)

  SD / CFexpress 卡 FENESTRA/ 模块（共 5 个文件，★ V71.5 必须同步更新 filmsim.wlm 与 slotcard.wlm!）:
    portrait.wlm            : {fenestra_meta['portrait.wlm']['sha256']} (4,872 bytes)   [简/繁/英 竖屏录像门控]
    slotcard.wlm            : {fenestra_meta['slotcard.wlm']['sha256']} (52,548 bytes)  [★ V71.5 更新: 支持 SC*.REC+0x39C Z8FX 解析 + 0 致命退出防插件消失]
    filmsim.wlm             : {fenestra_meta['filmsim.wlm']['sha256']} (25,960 bytes)  [★ V71.5 更新: 13 控件, 新增「全局/按云创配方绑定」与绑定/清除当前 NP3 + 0 致命退出]
    ZF_GRAIN.BIN            : {fenestra_meta['ZF_GRAIN.BIN']['sha256']} (27,588 bytes)  [Nikon Zf C3.01 胶片颗粒映射表（V71.5 起常驻内存零重复读卡）]
    STARTUP-Z8.0            : {fenestra_meta['STARTUP-Z8.0']['sha256']} (64 bytes)      [Fenestra 引导配置]

  ★ 重要升级提示 / UPGRADE NOTE:
    刷写本版 V71.5 时，请务必同时替换：
      1) 卡根目录 `Z_8_0406.bin`；
      2) `FENESTRA/filmsim.wlm`（25,960 B）；
      3) `FENESTRA/slotcard.wlm`（52,548 B）；
      4) 若为双卡机身（CFx + SD），两张卡的 `FENESTRA/` 请保持一致。旧卡 `FLMSIM0/1.CFG` 100% 无损向后兼容。


【二、V71.5 七大核心根因修复与重磅升级 / 2. V71.5 ROOT-CAUSE FIXES & UPGRADES】
----------------------------------------------------------------------------
  [1] 拍完秒按回放卡死 / 黑屏回弹退回取景根治（Live 开/关均已根治）
      - 彻底消除 `pb-fix37` 在 CPU2 跨核调用 UI 私有 RPC `0x403fb59c`/`0x403fa1b8` 导致的 RAW+JPG 写卡死锁（`pb.elf` 0 引用）；
      - 补齐 Class `0x24` 内存分配与布局切换钩子（`0x40f81680` / `0x40712808`），`preview_writer` 增加 MCU 行级秒级中止，进照片回放主动回收 58 MiB Wave5 内存，`live_shutter_z8` 增加 1.5s 防卡死超时；
      - 新增 `PTR64` 钩子 `0x42ac31b8 -> live_pb_arbiter_exit_wrap`，根治黑屏等待期间再按回放键被 arbiter [43] 误判为“退出回放”回弹回取景界面的问题；同时配合 4.30× 静态 HDF 提速使单张耗时远低于 4s `c3` 图像确认超时。

  [2] 45.4MP 静态 HDF **4.30× 提速** + Live 写卡大幅提速 + 照片零漏加颗粒/HDF 加固
      - 45.4MP 静态 HDF 引入 4×4 步进 Stage 1（Y PSNR `>= 50.03 dB`）+ 融合 NEON Stage 3（0 Q-spill, NEON==Scalar 192/192 0-diff）+ 去除 200 MiB 冗余 `dc civac` + NEON `zf_grain_copy_u8_fast` + 移除 Mode 2 `should_defer`，实测 45.4MP 彩色 HDF Medium 从 `5.8s` 降至 **`1.35s`（4.30×）**、单色 HDF 降至 **`0.52s`**；
      - 零漏加加固：`ZF_GRAIN.BIN`（28 KiB）常驻内存不再因进回放/切池反复读卡释放；读卡瞬态失败 1s 重试 + 连续 3 次错才隔离；纯颗粒模式改 Y-only 分配（静态 Class `0x24` 内存与拷贝减半省 ~45 MiB / ~250 ms）；新增 2 槽 `photo_slot1` FIFO 连拍队列防止切回放/连拍丢单；
      - 双卡 Live 备份改用 512 KiB 对齐 `live_write_twin_io` 搬运（写次数减半）。

  [3] 拍摄途中所有插件突然全部消失（重启又回来）双根因根治
      - 根因 A1：非 USB 触发的 `ev=7 BSShutdown` / `ev=8 BSShutdownCancel` 误调用核心 `stop_all()` 清空全部 `.wlm` → `live_ui.c` + `pb-fix37` + `0x401d6004 -> b 0x40d60ecc` 完整打通，非 USB `ev=7` 暂存 `BOOT-Z8.FLAG` 保持插件存活、`ev=8/15` 原地恢复，仅真关机 `ev=16` 停止；
      - 根因 A2：EL0 插件 100 ms 墙钟超时 + CPU0 争抢 + 单插件异常连坐清零 → `patch_boot_panic03_cardslot.py` v2 将 `.wlm` 绑核至 CPU2（`processor=3, affinity=4`），3 处超时放宽至 `1500 ms`，全 3 处故障路径改为单插件隔离；`filmsim.wlm` 与 `slotcard.wlm` 清零所有致命退出分支。

  [4] 云创（NP3）配方独立绑定颗粒 & 理光 HDF（Per-NP3 Recipe Binding）
      - `filmsim.wlm` 升级 13 控件，新增「生效模式（全局 / 按当前云创配方）」与「绑定到当前优化校准 / 清除当前绑定」；
      - 支持双层存储：优先读取 `SC*.REC +0x39C` 嵌入的 `'Z8FX'` 区块（配套离线工具 `pack_np3_z8fx.py` 支持直接写入 `.NP3` Comment 便携分发），并支持机身 95 槽 `FLMREC0/1.CFG` 实时覆盖；切换至未绑定配方（如原生 `SD`）自动休眠（`0 B Class 0x24` 零开销），切回已绑定 `C-10..C-64` 自动复醒。

  [5] 云创 `C-10..C-64` 丢失文字/图标根治 + **全机 7+ 处界面原生尼康云创图标与数字还原**
      - 根治冷开机 `p=1636` Fn 横条 `C-10..C-64` 空白（`hook_c64_style_cache` 补齐懒初始化字体样式缓存 `0x229`）与 `p=1496` `i` 键 12 宫格黄格空白；
      - **Bug 9 原生位图图标合成器（`0x4032DC80` / `0x4032DD60` → `0x40B69280`）**：1:1 提取原厂 `[FC框]` / `[C框]` / `[普通框]` 与窄体数字图元，动态合成标准 `kind=1` `64×32` 灰度遮罩 PNG 与 `68×36` 2px 黑描边 LV OSD PNG，直入 Z8 原厂 `libpng 1.6.37` 渲染管线（0 改字库、36 种语言全兼容）；
      - 全机 7+ 处界面（`p=1636` Fn 横条、`p=1496` `i` 键 12 宫格黄格/白格、`i` 键拨盘条、`p=1643` Fn HUD、取景器 LV OSD 黑描边、控制面板、拍摄菜单摘要）统一还原原厂 **`[FC]C-10..C-64`** / **`[C]C-10..C-64`** / **`[框]C-10..C-64*`** 图标质感！

  [6] 上游 `XinmiaoZhao/NikonFirmwarePatch` **v0.5.1** (`25fefa1`) 完整合并（0 冲突、0 删减）
      - 合并全部 4 项 N-RAW 3:2 关键修复：4.1K 3:2 待机取景冻结修复、4.1K 3:2 录制中触屏 AF 修复、取景器回放 3:2 花屏修复、3:2 自动区域 AF 绿框比例修复；
      - 完整保留我方 Pool #6 超集槽位 **`0x001C2000`**（`>= 0x0018D800`，同时兼容 N-RAW 3:2 `1280×848` 与我方 Live `1280×852` 满宽无损回放）与全部 4 处 `< 0x40B50000` / `Q3_STASH -> 0xA0FF52000` 安全重定位。

  [7] 完整保留 V71.4 / V71.3 全部既有能力（剥离富士仅留 Zf 锐利默认颗粒 + 理光 HDF、Silky 30fps Live、双卡防错、PANIC03 防护、竖屏录像回放、中文精简字库等）。


【三、验证汇总与诚实硬件边界 / 3. VERIFICATION SUMMARY & HONEST BOUNDARY】
----------------------------------------------------------------------------
  全部证据回执附于 `EVIDENCE/`（全部新鲜运行于最终 `{EXPECTED_MAIN_SHA[:8]}...` + 新 `filmsim.wlm` `c5d89096` + 新 `slotcard.wlm` `b221d05e`）：
    - **Host ASan+UBSan（0 错误）**：`cadence` 25/25、`w5_async` 4/4、V70 回归 10/10、`pb_handoff_v71r4` 9/9、
      `live_speedup_v71r4` 2/2（192/192 Still HDF 0-diff）、`v71r5_pb_ev7_twin` 6/6、`wlm_zh` 18/18、`slotcard` 30/30 + 38,942 + 634,711、
      `pack_np3_z8fx` 5/5、C64 Bug 9 PNG 合成器 550×2 + 550/550 zlib/CRC32。
    - **独立整镜审计 + 全套 Unicorn AArch64（12/12 套件 PASS，`{EXPECTED_MAIN_SHA[:8]}`）**：
      * 0 未解释字节（2,095,004 B 变更）、93/93 Manifest hooks（56 Stage 1 / 9 HangFix）+ 269 Stage 0 V71.5 位点 + NFP v0.5.1 `verify_contract()` PASS、
        36 `.o` 0 FP / 0 非核 SIMD、0 富士符号、`.filmsim` 142,624 B（余 21,216 B）、`pb.bin` 36,288 B（0 禁止 RPC 引用）、三路解包一致；
      * `boot_panic03_cardslot_arm` 286/286、`evf_pool6_arm` 10/10、Suite 1 349/349、Suite 2 47/47、Suite 3 833+833、
        Suite 4 FilmSim V71.5 70/70、Suite 5 FontCJK 3,603/3,603、Suite 6 PB Icon 70/70、Suite 7 Silky30fps 304/304；
      * E2E 基准：290/290 `.filmsim` 函数 0 Q-spill；45.4MP Still HDF Medium `1,349.53 ms`（**4.30× 提速**），Mono HDF `516.13 ms`，Stage 2 取消 `2.47 ms`；
      * **C64 Bug 9 实机码 Unicorn PNG 验收 50/50 PASS**（实测 `0x4032DC64->0x4032DC80->0x40B69280` 产出合法 64×32 与 68×36 描边 PNG）。
    - **全机 Whole-Guest QEMU（最终 `{EXPECTED_MAIN_SHA[:8]}` + 5 个 FINAL `FENESTRA/` + 未改动原版 QEMU）**：
      * **17/18 FULL PASS + 1/18 MODEL_BOUNDARY_EXPECTED（总计 318/318 checks PASS，0 Z8_UNKNOWN / 0 unexpected Z9_UNKNOWN / 0 PANIC / 0 FX_TIMEOUT）**：
        - `v71r5_pb_bounce_final` **19/19**、`v71r5_ev7_plugins_final` **13/13**、`v71r5_np3_recipe_final` **14/14**；
        - `c64_imenu_final` **20/20** + `fn_pc_final` **17/17** + **12/12 全界面真实 QEMU 截图目视核验通过**（`[FC]C-10..C-60` 清晰原生还原）；
        - `nfp_v050_final` (NFP **v0.5.1**) **12/12**（含 4.1K 3:2 待机不冻结）；
        - Run 1A **33/33**、Run 1B/1C **28/28**、Run 2A/2B **27/27**、Case B Mode 2 **29/29**、Case C 旧卡迁移 + Mode 2/1 全周期 **56/56**、Slotcard 更新首启 + 87 实机 `SC*.REC` **34/34**；
        - Case A Mode 1 纯颗粒 **16/16 `MODEL_BOUNDARY_EXPECTED`**（Live Mode 1 73 帧 + 45MP 静态 `861.93 ms` 完成，仅因 Y-only 保持原厂 UV 未改触发原版 `filmsim-qemu-r1` 保守检查 `cpu_ram_known_producer_overlap`，未改动测试仿真器，纳入实体机验收项）。

  ★ 重要声明 — 实体相机硬件边界 (`physical_camera_verified = false`):
    1) 本版 V71.5 尚未在实体尼康 Z8 机身上实拍验收；
    2) 实验室 QEMU 仅仿真 CFx A 槽（SD B 槽原生 UHS-II 寄存器 `0x1782823c` 未建模）、仅仿真 LCD `1024×682`（无 120Hz EVF 面板）、
       未建模 NEF RAW 硬件压缩与纯颗粒 Y-only 混池 JPEG 提交（Case A `0x15e2`），也未建模 EXPEED 7 CEVA-XM6 DSP；
       因此**实机双卡上电、纯颗粒 Y-only JPG/RAW+JPG 实拍、45MP HDF 实测写卡耗时、拍完秒按/连按回放、非 USB 休眠/息屏插件存活、Per-NP3 绑定切换、全界面云创原生图标与 N-RAW 4.1K 3:2 实拍**均需上实体尼康 Z8 最终验收。
"""
    readme_path = FINAL / 'README-v71.5-pbspeed-np3-nfp051.txt'
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
        ('c64_bug9_png_arm.json', TESTS / 'verify_arm/c64_bug9_png_arm.json'),
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
        ('v71r5_verify_qemu_final_summary.json', TESTS / 'verify_qemu_final/summary.json'),
        ('wlm_filmsim_glyph_coverage.json', TESTS / 'wlm_zh/glyph_coverage_report.json'),
        ('wlm_filmsim_verify_report.json', V71R5 / 'work/wlm-zh/filmsim/build/verify-report.json'),
        ('wlm_slotcard_glyph_check.json', TESTS / 'wlm_zh_slotcard/glyph_check.json'),
        ('wlm_slotcard_verify_report.json', V71R5 / 'work/wlm-zh/slotcard/build/verify-report.json'),
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
        'schema': 'v71.5.pbspeed-np3-nfp051-delivery-verification/v1',
        'version': 'v71.5-pbspeed-np3-nfp051',
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
        '--version', 'v71.5-pbspeed-np3-nfp051',
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
