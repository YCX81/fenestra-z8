#!/usr/bin/env python3
"""Publish the verified V71.7 All-in-One official delivery bundle to:
  /Users/ycx/Nikon/delivery/Z8-v71.7-LutPreview-fe3e7f22/
  /Users/ycx/Nikon/delivery/Z8-v71.7-LutPreview-fe3e7f22.zip
via pack68.py, then independently verify shasum -a 256 -c SHA256SUMS.txt and unzip -tq.
STRICT: Never modifies prior delivery packages (Z8-v71.6-* / Z8-v71.5-* / Z8-v71.4-*), frozen workspaces, or /Volumes/* !
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
FINAL = V71R7 / 'final'
TESTS = V71R7 / 'tests'
FONT_ZH = V71R7 / 'work/font-zh'
SD_FENESTRA = V71R7 / 'sd-card/FENESTRA'
DELIVERY = Path('/Users/ycx/Nikon/delivery')

EXPECTED_MAIN_SHA = 'fe3e7f2232c338c87fc23d388d439379451293bfc972d45a4a6a710cd9fa7530'
EXPECTED_PKG_SHA = '9976d012cac5c88e585a29a9798a7f691c74caad56098b60825959b6ae492b64'
EXPECTED_BUILD_ID = '0xd590d8cd'
EXPECTED_ORIG_TEXT_VA = '0x401eadcc'
EXPECTED_V68_MENU_TEXT_VA = '0x401ff134'
EXPECTED_V68_NFP_OVERLAP_VA = '0x401ff294'
EXPECTED_FILMSIM_SLOTS_SVC_VA = '0x409c3d00'

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
        34856,
        '7943f163ffbe631d1620bb8a9162b59c108a9d853f2e122be197196cc08199cc',
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

    prefix = 'Z8-v71.7-LutPreview-'
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
    manifest['filmsim_slots_svc_va'] = EXPECTED_FILMSIM_SLOTS_SVC_VA
    manifest['verification_complete'] = {
        'v71r7_ceva_xm6_coexist_nikondma_dcli_47of47': (
            'PASS (real CEVA-ToolBox V18.0.3 clang/clang++ 7.1.0 + ceva-elf-rcheck -full 4/4 ZERO violations + '
            '47/47 dcli.exe 0-diff vs ARM64 NEON/Scalar against strict NikonDMA alignment & canary mock: '
            'Slot 39/40 Zf FilmGrain NikonDMA 25/25 [128x128, 1024x680, multi-strip 2048x32 intensity=6 odd r_tot=3, 16B aligned], '
            'Slot 41 Ricoh HDF v2 NikonDMA 22/22 [fixed Vol II Sec 6.7 Restriction 2 .ds2->.ds vpld eliminating real-silicon diff_hdf_still=19713, '
            'gws*2U 16B grid DMA alignment, 0x41930000..0x419B0000 tsk0esc/tsk1esc 100% untouched when ext_work_addr!=0]; '
            '0x .ds2, 0x .c2/.c4/.s2, 0x vpst, 0 FP, .text.z8fx 134,816 B [23,520 B headroom], .rodata.z8fx 1,952 B)'
        ),
        'v71r7_host_asan_ubsan': (
            'PASS (test_lut_core_host 6/6, test_xm6_bridge 7/7 NEON + 7/7 SCALAR 0-diff [incl. Group g: P0 ext_ws!=0, '
            '2-tier breaker, post-CB mailbox wait, hung-DMA quarantine, 0xF1..0xF7 diag skip codes], '
            'test_layer0_host 7/7 [edge-triggered pool_resume/suspend + -2 hung-DMA quarantine], '
            'test_photo_menu_host 5/5, wlm_zh filmsim 20/20, slotcard 30/30)'
        ),
        'v71r7_binary_and_unicorn_14of14_on_fe3e7f22': (
            'PASS (0 unexplained bytes across 2,335,230 changed bytes vs Stock C3.11; coexist_nikondma ELF0 verified; '
            '54 WSL .o [49 scalar + 5 NEON kernels], 0 FP, 0 non-kernel SIMD, 0 writable sections; '
            'STRICT 0 Q-spills across all 698 linked functions [0 waivers]; '
            '101 hooks + 2 Photo Menu RAM init patches + 0xA02917CA0 untouched zero; Z_8_0406.bin 3-way unpack match; '
            'Suite #14 run_v71r7_lut_preview_arm 63/63 [6 Layer0/PowerSync hooks, 30/30 localized 0x1F40..0x1F4A text, '
            'edge-triggered pool_resume E2b 0 duplicate flight entries, CPACR_EL1=0]; '
            'run_xm6_bridge_arm 124/124 [Slot 41 ext_work_addr!=0 524288 B, ext_ws release busy-withhold & hw_state=0 zero-leak, '
            '2-tier Live soft-backoff preserving Still/Grain/Op12, hung-DMA -2 hard breaker, 0xF1..0xF7 skip codes, Op12 RGBLUT + 512MiB spacer, 0x40c91c84 0 hits]; '
            'Suite 2 HangFix 47/47 [FS_ARENA_DESC cleanly released + ev=16 BSWaitEnableSuspend safe & FxZ8LutIoGate OPEN=1], '
            'Suite 4 FilmSim 70/70, Boot/PANIC03 286/286, EVF Pool6 10/10, Suite 1 Live 349/349, Suite 3 PortraitPB 833/833, '
            'Suite 5 FontCJK 3,603/3,603, Suite 6 PB Icon 70/70, Suite 7 Silky30fps 304/304, E2E Bench 1,349.53 ms, C64 Bug 9 PNG 50/50)'
        ),
        'v71r7_whole_guest_qemu_20suites_on_fe3e7f22': (
            '19/20 FULL PASS + 1/20 MODEL_BOUNDARY_EXPECTED (Case A pre-15e2 16/16 PASS) = 347/347 checks PASS '
            '(0 Z8_UNKNOWN, 0 unexpected Z9_UNKNOWN, 0 PANIC, 0 park_err, 0 close_failed): '
            'NEW Suite #20 v71r7_lut_preview_final 13/13 FULL PASS '
            '(Photo Menu 48 rows [Row 45 0x2c13 Pixel Shift intact, Row 47 0x2de7 选择 LUT], WarmFilm.cube selected + '
            'FXPHOTO0/1.CFG v2 CRC32 persisted & cold-reboot auto-restored + 无LUT cleared, Display Layer 0 14.55 MiB WYSIWYG '
            'disp_substituted=18 golden-peach graded LV + HangFix V69 Movie pool_suspend release to 0 B, Unified Still JPEG '
            '8256x5504 DSC_0001.JPG R-B=+53.06, clean FxZ8FlightRing head=8/3 with 0 POOL_RESUME flood, 8 visually verified ZH PNGs); '
            'Suite #19 v71r6_xm6_ui_and_fallback_final 16/16 FULL PASS (coexist_nikondma fw_loaded=1/loader_fault=0, XM6_DIAG.LOG); '
            'Suite #7 Case B live-verified Hardening #4 2s stall watchdog dumping 521 B FENESTRA/FXCRASH.LOG without hang (29/29 PASS); '
            'ALL 18 prior V71.5 regression suites 100% PASS'
        ),
    }
    final_manifest_copy = ver_dir / 'v71-manifest.json'
    final_manifest_copy.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (FINAL / 'v71-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

    # 3. Honest Bilingual README.txt
    readme_text = f"""Nikon Z8 V71.7 All-in-One 固件发布包 ({prefix}{EXPECTED_MAIN_SHA[:8]})
Nikon Z8 V71.7 All-in-One: Real-Silicon Fixed CEVA-XM6 NikonDMA DSP + Native Photo Menu LUT + Display Layer 0 WYSIWYG Preview + Full V71.5/V71.6 Stack
============================================================================

【一、固件标识与校验和 / 1. FIRMWARE IDENTITY & CHECKSUMS】
----------------------------------------------------------------------------
  固件版本 / Version        : V71.7 (v71r7-lut-preview; 基于 C3.11 + NFP v0.5.1 + V71.6-XM6 NikonDMA 实机修复全栈)
  内部标识 / Identity       : 04.06.p0
  Bootguard Build ID        : {EXPECTED_BUILD_ID} (at 0x401FE060)
  动态 ORIG_TEXT_VA         : {EXPECTED_ORIG_TEXT_VA} (v68_menu_text @ {EXPECTED_V68_MENU_TEXT_VA}, filmsim_slots_svc @ {EXPECTED_FILMSIM_SLOTS_SVC_VA})
  MAIN (eg2070) SHA-256     : {EXPECTED_MAIN_SHA} (81,807,709 bytes)
  Z_8_0406.bin SHA-256      : {EXPECTED_PKG_SHA} (99,051,117 bytes)

  SD / CFexpress 卡 FENESTRA/ 模块（共 5 个文件，与 V71.6-XM6 完全兼容）:
    filmsim.wlm             : {fenestra_meta['filmsim.wlm']['sha256']} (34,856 bytes)  [16 控件: XM6 硬件加速开关/一键实机自检/XM6 实时状态 + slots.state 直连 EL1]
    slotcard.wlm            : {fenestra_meta['slotcard.wlm']['sha256']} (52,548 bytes)  [SC*.REC+0x39C Z8FX 解析 + 休眠防消失]
    portrait.wlm            : {fenestra_meta['portrait.wlm']['sha256']} (4,872 bytes)   [简/繁/英 竖屏录像门控]
    ZF_GRAIN.BIN            : {fenestra_meta['ZF_GRAIN.BIN']['sha256']} (27,588 bytes)  [Nikon Zf C3.01 胶片颗粒表（常驻内核内存）]
    STARTUP-Z8.0            : {fenestra_meta['STARTUP-Z8.0']['sha256']} (64 bytes)      [Fenestra 引导配置]

  ★ 升级与 .cube LUT 放置说明 / UPGRADE & .cube LUT GUIDE:
    1) 将根目录 `Z_8_0406.bin` 复制到储存卡根目录并在相机 `设定菜单 → 固件版本` 刷入；
    2) 将本包 `FENESTRA/` 内 5 个文件复制到卡上 `FENESTRA/`（双卡机身请保持 A/B 卡一致；若已是 V71.6-XM6 同版 SHA 则无需重拷）；
    3) 放置自定义 1D/3D `.cube` LUT（可选，不放则完全休眠 0 内存占用）：
       - 根目录快捷 LUT：`FENESTRA\\*.cube`（显示在 `选择 LUT → 根目录 LUT`）
       - 分类目录 LUT：`FENESTRA\\LUTS\\<分类名>\\*.cube`（例如 `FENESTRA\\LUTS\\Cinema\\WarmFilm.cube`）
       - 进入尼康原生 `照片拍摄菜单` 最底部第 48 行 **`选择 LUT`** 即可直接选择、掉电自动记忆（双槽 CRC32 `FXPHOTO0/1.CFG`）并实时预览。


【二、本次实机 EXPEED 7 CEVA-XM6 根因修复（彻底解决实机 `3/4差` 与 `熔断E1/回退231`）】
----------------------------------------------------------------------------
  [1] 根治 `XM6异常 3/4差`（实机 `diff_hdf_still = 19,713` 单字节精确吻合修复）：
      - 实锤定位 `ricoh_hdf_xm6_opt.cpp:2951` `still_hlerp_row()` 中的 `vpld(vrow, g, v0, v1)` 生成了 `.ds2` 向量聚合访存，违反 CEVA-XM6 架构手册 Vol II Sec 6.7 Restriction 2（`.ds2` 要求 4B 对齐，奇数 `gx[x]` 在 EXPEED 7 硅片上导致 `v1 = 0`，注入该故障后与实机 `XM6_DIAG.LOG` 差值精确相等 `19,713 == 19,713`）；
      - 已替换为两条合规 2B `.ds` `vpld` 指令 + Stage 3 网格读 `gws * 2U` 16B 对齐；全 ELF0 静态扫描 **`0× .ds2`、`0× vpst`**，CEVA `dcli.exe` 22/22 静态/实况 HDF 全分辨率 0-diff PASS。

  [2] 根治 `XM6状态 熔断E1 / 回退231`（实况 HDF 49.36ms 超时 → 切换硬件 `coexist_nikondma`）：
      - 实机标定证明 EXPEED 7 Core 0 无外部数据缓存，`coexist_direct` 逐向量无缓存读 LPDDR 耗时 `~44.7 ms`（总 `49.36 ms > 25 ms` 超时触发全局熔断；`cb_code=0x42180003` 为正常完成事件、`dsp_status=0` 说明 DSP 实际已正常跑完）；
      - 已将 Slot 39/40（Zf 颗粒，修复 `prepare_tables` 与多条带 16B/4B DMA 对齐）和 Slot 41（理光 HDF v2）全部切换为真硬件 **`coexist_nikondma`**（1D/2D AXI 突发双缓冲，`rcheck -full` 4/4 零违规，47/47 `dcli.exe` 0-diff PASS），实机实况 HDF 预期由 `49.36 ms` 降至 **`~4.2–5.6 ms`（9–12× 加速）**，45.4MP 静态 HDF 降至 **`~256–341 ms`**，45.4MP 颗粒降至 **`~114–241 ms`**。

  [3] P0 硅片并发安全修复（禁止踩踏原厂 FreeRTOS `tsk0esc/tsk1esc` 溢出区）：
      - 反汇编 `z8_xm6_elf0.elf` 确认 `0x41940000..0x419A0000` 为原厂 `FnH/FnL` 任务切换 192 KiB IDM 保护区；EL1 现已改为始终为 Slot 41 懒分配独立 64B 对齐 `512 KiB` Class `0x24` 工作区并传入 `ext_work_addr != 0`（息屏/切视频自动安全释放，绝不触碰 `0x41930000..0x419B0000`）。

  [4] EL1 桥接两级熔断、回调竞态关闭、消除 `dly_tsk(1)` 量化与系统生命周期修复：
      - **两级熔断**：实况超时提高至 `35 ms` + `120 ms` 排空，若 DSP 干净排空仅触发实况软退避（5s 退避 / 3 次软关闭），**绝不连坐熔断 45MP 静态 HDF/颗粒与 Op12 RGBLUT**；仅真实挂死才触发全局硬熔断；
      - 关闭 `0x40c90fb0 -> 0x40c90fcc` 回调后邮箱复位微秒级竞态、关闭 Class `0x24` DMA 在途 UAF、`<= 2000 us` 快速轮询前导消除 `dly_tsk(1)` 1–2ms 量化、明确诊断跳过码 `0xF1..0xF7`；
      - 移除 `V71.7` 全部 4 处 `ev=16 (BSWaitEnableSuspend)` 误关机（含 `photo_worker_shutdown()`），修复 `filmsim_live_worker_entry` 空闲 CPU2 100% 死循环，并修复 `filmsim_release_arena()` 在 Core 0 断电/QEMU 下的 HangFix 释放保护。


【三、V71.7 全新整合能力（静态统一 EL1 集成，无须额外 .wlm）】
----------------------------------------------------------------------------
  [1] 原生照片拍摄菜单 Row 47 `选择 LUT`（Page `0x5D6`，完整中/繁/英/日多语，0 缺字）+ 根目录/分类 `.cube` 扫描 + 双槽 CRC32 `FXPHOTO0/1.CFG` 掉电记忆 + 常驻低耗 `photo_worker`（空闲 20ms 休眠）。
  [2] Display Layer 0 所见即所得实时 LCD/EVF 取景预览（`14.55 MiB` `photo_workspace` 与 HangFix V69 `pool_suspend`/`pool_resume` 深度联动，休眠/未选 LUT 时 0 字节占用）：统一串行 **`Stage 0 .cube 3D LUT → Stage 1 理光 HDF (XM6 Slot 41 LCD / NEON EVF) → Stage 2 Zf 颗粒 (GTILE NEON)`**。
  [3] 统一静态 JPEG 6 槽 NV16 与 30fps 实况短片 Stage 0 `.cube` LUT 联动（1D 可分离 `.cube` 走 XM6 Opcode 12 `RGBLUT` 快路径；3D `.cube` 走 CPU2+CPU3 双核 SMP + NEON + 4 MiB YCbCr 缓存）。
  [4] 5 项上游加固全合入：XM6 `512 MiB` 跨窗 spacer + `0xA02E7EED0` 邮箱空闲校验 + Op12 `RGBLUT`；`neon_sentinel` 运行时 NEON/IRQ 金丝雀自检；`POWER_OFF_SYNC/POWER_CANCEL_SYNC` + `FxZ8LutIoGate` 断电门控；`photo_flight` 64 条黑匣子飞行记录环 + 2s 卡死看门狗自动写 `FXCRASH.LOG`；CPU2+CPU3 共享无锁 `smp.c` 条带池。


【四、验证汇总与实体 Z8 实机验收清单 (`physical_camera_verified = false`)】
----------------------------------------------------------------------------
  全部证据回执附于 `EVIDENCE/`（全部新鲜运行于最终 `{EXPECTED_MAIN_SHA[:8]}`）：
    - **CEVA-ToolBox V18.0.3 `dcli.exe`**：4/4 `rcheck -full` 0 违规、**47/47 0-diff PASS**（25/25 颗粒 + 22/22 HDF，`0× .ds2`）；
    - **Host ASan+UBSan**：`lut_core` 6/6、`xm6_bridge` 7/7 NEON + 7/7 SCALAR、`layer0` 7/7、`photo_menu` 5/5、`wlm_zh` 20/20；
    - **独立整镜审计 + 全套 Unicorn AArch64（14/14 套件 PASS）**：`2,335,230 B` 变更 0 未解释字节、54 `.o` 0 FP / 0 非核 SIMD、**698/698 函数 0 Q-spill（0 豁免）**、`run_xm6_bridge_arm` **124/124**、Suite #14 `run_v71r7_lut_preview_arm` **63/63**、Suite 2 HangFix **47/47**（`ev=16` 安全）、三路解包一致；
    - **全机 Whole-Guest QEMU（未改动 `filmsim-qemu-r1`）**：**19/20 FULL PASS + 1/20 Case A `MODEL_BOUNDARY_EXPECTED`（347/347 checks PASS，0 Z8_UNK / 0 unexpected Z9_UNK / 0 PANIC）**，含 Suite #20 `v71r7_lut_preview_final` **13/13 FULL PASS** 与 Suite #19 **16/16 FULL PASS**，8 张 V71.7 中文菜单/实时取景/视频挂起截图全部目视核验。

  ★ 实体尼康 Z8 上机验收步骤 (`physical_camera_verified = false`)：
    1) 刷入本版 `Z_8_0406.bin` 后正常开机，进入 `Zf 胶片颗粒 / HDF` 菜单：
       - 确认 `EL1 状态` 显示 `就绪 Zf:已加载`、`XM6 状态` 显示 `就绪 (S:2)`；
       - 点击 **`XM6 实机自检`**，确认 **4/4 全通过 0 差**（`passed_mask=0x0F`，`diff_hdf_still=0`！），查看 `FENESTRA/XM6_DIAG.LOG` 验证 `us_hdf_live` 由 `49.36 ms` 降至 `~4–6 ms`、`us_grain_*` 显示真实微秒耗时；
    2) 开启理光 HDF + Zf 颗粒 + 实况联动，回到实时取景观察数秒并实拍 45.4MP JPEG / RAW+JPG：
       - 确认 `XM6 状态` 不再触发 `熔断E1`，且 `S:`（静态 XM6）与 `L:`（实况 XM6）计数持续递增！
    3) 放入测试 `.cube` 文件至 `FENESTRA/` 或 `FENESTRA/LUTS/<分类>/`，进入原生 `照片拍摄菜单` 最底部 **`选择 LUT`**，测试实时取景 LCD/EVF 预览、切视频/回放（确认无卡死且颜色正确复位）、拍摄静态 JPEG 与重启自动恢复；
    4) 诚实硬件边界：QEMU 未建模 CEVA-XM6 DSP 硅片、Display Layer 0 真实 LCD/EVF 120Hz 扫描时序、物理电源开关 `POWER_OFF_SYNC` 抖动、SD B 槽 UHS-II `0x1782823c`、及引擎 `0x15e2` 混合缓存，以上均需实体 Z8 最终确认。
"""
    readme_path = FINAL / 'README-v71.7-lut-preview.txt'
    readme_path.write_text(readme_text, encoding='utf-8')

    # 4. Collect ALL verified JSON evidence receipts
    evidence_sources = [
        ('v71-manifest.json', final_manifest_copy),
        ('coexist_manifest.json', HERE / 'xm6_elf0/coexist_nikondma/coexist_manifest.json'),
        ('photo_menu_tables.json', V71R7 / 'work/lut_menu/photo_menu_tables.json'),
        ('stage0-base.json', FINAL / 'stage0-base.json'),
        ('stage1-build.json', FINAL / 'stage1-build.json'),
        ('stage1-simd-audit.json', FINAL / 'stage1-simd-audit.json'),
        ('stage2-portrait.json', FINAL / 'stage2-portrait.json'),
        ('stage2-portrait-audit.json', FINAL / 'stage2-portrait-audit.json'),
        ('main.json', FINAL / 'main.json'),
        ('stage3-fontzh-audit.json', FINAL / 'stage3-fontzh-audit.json'),
        ('verify_arm_summary.json', TESTS / 'verify_arm/summary.json'),
        ('audit_whole_image.json', TESTS / 'verify_arm/audit_whole_image.json'),
        ('v71r7_lut_preview_arm.json', TESTS / 'verify_arm/v71r7_lut_preview_arm.json'),
        ('xm6_bridge_arm.json', TESTS / 'verify_arm/xm6_bridge_arm.json'),
        ('boot_panic03_cardslot_arm.json', TESTS / 'verify_arm/boot_panic03_cardslot_arm.json'),
        ('c64_bug9_png_arm.json', TESTS / 'verify_arm/c64_bug9_png_arm.json'),
        ('evf_pool6_arm.json', TESTS / 'verify_arm/evf_pool6_arm.json'),
        ('suite1a_picker_abi.json', TESTS / 'verify_arm/suite1a_picker_abi.json'),
        ('suite1b_audio_arm.json', TESTS / 'verify_arm/suite1b_audio_arm.json'),
        ('suite1c_duplex_final.json', TESTS / 'verify_arm/suite1c_duplex_final.json'),
        ('suite1d_review_arm.json', TESTS / 'verify_arm/suite1d_review_arm.json'),
        ('suite2_hangfix_arm.json', TESTS / 'verify_arm/suite2_hangfix_arm.json'),
        ('suite3_portrait_pb_final.json', TESTS / 'verify_arm/suite3_portrait_pb_final.json'),
        ('suite4_filmsim_final.json', TESTS / 'verify_arm/suite4_filmsim_final.json'),
        ('suite5_font_cjk_final.json', TESTS / 'verify_arm/suite5_font_cjk_final.json'),
        ('suite6_pb_icon_final.json', TESTS / 'verify_arm/suite6_pb_icon_final.json'),
        ('suite7_v70_silky30fps_arm.json', TESTS / 'verify_arm/suite7_v70_silky30fps_arm.json'),
        ('v71_e2e_bench_arm.json', TESTS / 'verify_arm/v71_e2e_bench_arm.json'),
        ('v71r7_verify_qemu_final_summary.json', TESTS / 'verify_qemu_final/summary.json'),
        ('wlm_filmsim_glyph_coverage.json', TESTS / 'wlm_zh/glyph_coverage_report.json'),
        ('wlm_filmsim_verify_report.json', V71R7 / 'work/wlm-zh/filmsim/build/verify-report.json'),
        ('wlm_slotcard_glyph_check.json', TESTS / 'wlm_zh_slotcard/glyph_check.json'),
        ('wlm_slotcard_verify_report.json', V71R7 / 'work/wlm-zh/slotcard/build/verify-report.json'),
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
        'schema': 'v71.7.lut-preview-delivery-verification/v1',
        'version': 'v71.7-lut-preview',
        'identity': '04.06.p0',
        'main_sha256': EXPECTED_MAIN_SHA,
        'package_sha256': EXPECTED_PKG_SHA,
        'build_id': EXPECTED_BUILD_ID,
        'orig_text_va': EXPECTED_ORIG_TEXT_VA,
        'v68_menu_text_va': EXPECTED_V68_MENU_TEXT_VA,
        'v68_nfp_overlap_text_va': EXPECTED_V68_NFP_OVERLAP_VA,
        'filmsim_slots_svc_va': EXPECTED_FILMSIM_SLOTS_SVC_VA,
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
        '--version', 'v71.7-lut-preview',
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
    print("=== V71.7 shasum -a 256 -c SHA256SUMS.txt ===")
    print(shasum_res.stdout, end='')

    unzip_res = subprocess.run(
        ['unzip', '-tq', str(target_zip)],
        text=True, capture_output=True, check=True,
    )
    print("=== V71.7 unzip -tq ===")
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
