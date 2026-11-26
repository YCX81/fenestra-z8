# 实况照片与胶片模拟套件（V71.7-r7）/ Live Photo & FilmSim Suite (V71.7-r7)

> 本功能组基于官方 Nikon Z8 C 3.11 固件，完整包含重定位后的 NFP v0.5.1 全部四组功能，并叠加 V71.7-r7 实况照片、胶片模拟、实时 3D LUT 预览、CEVA-XM6 颗粒与光学柔焦（HDF）、人像调色、CJK 扩展字库、双卡槽管理及 C64 i 菜单插件体系。

## 功能概览

- **实况照片（Live Photo）**：快门前后自动捕获动态画面；V71.7-r7 引入快门后 45 MP 静图顺延机制，消除实况短片后半段帧率冻结。
- **胶片模拟与实时 3D LUT 预览（FilmSim & LUT）**：支持机内实时 3D LUT 取景预览与照片直出，采用 528 KiB L2 常驻预烘焙 64×64×33 YCbCr 3D LUT 与反向 Layer 0 → Live 单通道共享。
- **CEVA-XM6 硬件加速（Grain & HDF）**：CEVA-XM6 Core 0 共存部署 Zf 胶片颗粒（Slot 39/40）与 Ricoh 光学柔焦 HDF v2（Slot 41），并启用锁前排队以消除邮箱冲突回退。
- **人像、双卡与字库支持**：内建人像调色插件、双卡槽路由（CFexpress / SD）与简体／繁体中文扩展字库，并完整保留重定位后的 NFP v0.5.1 四组功能（视频录制、Log 色彩、照片工具、RAW 预拍摄）。

## 存储卡部署（FENESTRA/）

构建本功能组时，输出目录会同时生成固件 `Z_8_NNNN.bin` 与配套的 `FENESTRA/` 目录（共 6 个开源运行时文件）：

- `FENESTRA/STARTUP-Z8.0`
- `FENESTRA/filmsim.wlm`
- `FENESTRA/livephoto.wlm`
- `FENESTRA/lut.wlm`
- `FENESTRA/portrait.wlm`
- `FENESTRA/slotcard.wlm`

请将 `Z_8_NNNN.bin` 与整个 `FENESTRA/` 文件夹一并复制到存储卡根目录，然后通过机身「设定菜单 › 固件版本」完成升级。如需启用 Zf 银盐颗粒，请自行准备 `ZF_GRAIN.BIN` 并放入存储卡 `FENESTRA/` 目录（本公开仓库与工具包不分发专有素材）。

## 限制与注意事项（触发条件与后果）

- **拍摄实况必须等 `◎LIVE` 保存结束再操作（防黑屏／死机／丢片）**：按快门后在 `◎LIVE` 采集后半段（约 1.5 秒）与闪烁／`Busy` 写入封装期间，**切勿立即切视频模式、反复按回放键、关机或拔卡**；强行打断 Wave5 硬件交接会丢弃本次实况 `.MOV`（及双卡副卡副本），极端情况下可能造成黑屏、回放无响应或卡死（需拔电池并触发防变砖保护）。进入取景后也请等待约 1.5 秒待 `◎LIVE` 图标常亮就绪再按快门。
- **实况画质必须含 JPEG**：纯 `.NEF` 或 `SDR/HLG HEIF`（`.HIF`）不会生成配对实况短片，请设为 `JPEG` 或 `RAW + JPEG`。
- **EVF 取景全开掉帧与卡顿**：在取景器（EVF，1280×852）下同时开启实时 3D LUT 预览 + 光学柔焦 HDF + 颗粒 + 实况照片时，单帧处理超过 16.7 ms（60 Hz）预算，EVF 会出现明显掉帧与卡顿（背屏 1024×682 基本可用）。建议 EVF 取景时关闭「取景实时预览」或关闭 HDF／颗粒中的 1–2 项。
- **实况照片 `.MOV` 掉帧与重复帧**：开启实况照片并同时叠加 3D LUT / HDF / 颗粒（尤其是 EVF 取景拍摄）时，由于 Layer 0 异步推帧与编码宽限窗失配或共享回退，导出的 `.MOV` 会出现明显掉帧与重复帧；上一张 `◎LIVE` 处于 `Busy` 保存期间连按快门仅保存静图，不附带配对实况短片。
- **先开 3D LUT 后开 HDF 的 XM6 回退**：先分配 3D LUT 工作区后再新开 HDF，若缓冲跨 512 MiB 边界将回退 ARM NEON 导致降速；在 `lut.wlm › 调试设置 › 重置预览工作区` 或重启机身即可恢复。
- **必须同时部署 `FENESTRA/` 全部 6 个开源文件到存储卡根目录（颗粒需自备 `ZF_GRAIN.BIN`）**：缺少 `.wlm` / `STARTUP-Z8.0` 会导致插件无法加载；缺少 `ZF_GRAIN.BIN` 仅颗粒不生效；停用 `.wlm` 需重启生效。
- **3D LUT 作用范围与录像挂起**：仅作用于取景预览、JPEG 与实况，14-bit `.NEF` RAW 严格 0 改动；进入视频模式自动释放预览池为 `0 B`，切回照片模式自动恢复。
- **自动包含重定位后的全部四组 NFP v0.5.1 功能**：`Pool #6` 输出槽扩展为 `0x001C2000`；当前 `V71.7-r7` 处于真机性能优化阶段，刷入前请做好原厂设置备份，勿用于不可替代的工作。

---

## English

### Overview

This feature group builds on official Nikon Z8 C 3.11, includes all four relocated NFP v0.5.1 feature groups, and adds the V71.7-r7 suite:

- **Live Photo**: Captures pre- and post-shutter motion clips with post-shutter 45 MP still deferral to keep post-roll frame rate smooth.
- **FilmSim & Real-time 3D LUT Preview**: In-camera real-time 3D LUT preview and rendering using a 528 KiB L2-resident pre-baked 64×64×33 YCbCr 3D LUT and reverse Layer 0 → Live single-pass sharing.
- **CEVA-XM6 Acceleration**: Coexisting Zf Film Grain (Slots 39/40) and Ricoh Optical HDF v2 (Slot 41) on CEVA-XM6 Core 0 with lock-aware mailbox queueing.
- **Portrait, Dual-Slot & CJK Font**: Bundled portrait plugin, dual-slot card routing, CJK extended fonts, and C64 i-menu integration alongside all four relocated NFP v0.5.1 groups.

### Card Deployment (`FENESTRA/`)

Building this group exports both `Z_8_NNNN.bin` and the `FENESTRA/` directory containing all 6 open-source runtime files (`STARTUP-Z8.0`, `filmsim.wlm`, `livephoto.wlm`, `lut.wlm`, `portrait.wlm`, `slotcard.wlm`). Copy both `Z_8_NNNN.bin` and the entire `FENESTRA/` folder to the root of your memory card before updating firmware in the Setup menu. If you want Zf film grain, supply your own `ZF_GRAIN.BIN` in `FENESTRA/` on the memory card (not distributed in this repository or toolkit).
