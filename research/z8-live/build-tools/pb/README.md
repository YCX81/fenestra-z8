# 机内 Live 回放层（live-playback-ui）重建说明

回放层位于 `0x40196000–0x401a0000`，hook 共 10 处（MCU 按键、system、删除、保护、设置菜单、离开/状态、UI poll/wait）。源码在 `live-playback-ui/`，取自 `_bundle-20261102/codex-tasks/live-playback-ui`；`live-playback-ui.orig/` 是改动前的原样。

## 平铺布局（2026-11-05）
原生照片 `A:\DCIM\100NCZ_8\DSC_0001.JPG` 对应的四个文件：

| 文件 | 路径 | 用途 |
|---|---|---|
| 配对照片 | `A:\LIVE\100NCZ_8\DSC_0001.JPG` | 导入（带配对标识） |
| 视频 | `A:\LIVE\100NCZ_8\DSC_0001.MOV` | 导入 |
| 预览 | `A:\LIVEDATA\100NCZ_8\DSC_0001.LVP` | 机内回放 |
| 清单 | `A:\LIVEDATA\100NCZ_8\DSC_0001.MAN` | 回放校验、删除/保护同步、开机 GC |

- `sync.c`：`live_sync_directory()` 现在返回数据 stem `A:\LIVEDATA\100NCZ_8\DSC_0001`；`live_sync_file(out, native, kind)` 给出 4 个完整路径。删除/保护按 JPG→MOV→LVP→MAN 的顺序处理（MAN 最后删，作为恢复锚点）。没有每张照片一个的文件夹，也就不再删除文件夹。
- `native_sync.c`：只允许写这 4 个由 stem 推出的路径；开机 GC 只认 `LIVEDATA\folder\stem.MAN`。
- `runtime.c`：回放查找直接用 `live_sync_file()` 拿 MAN 和 LVP。
- 写入端（Live payload）的同样改动在 `build-tools/w5/flat_patch.py`。
- 旧布局（`LIVE\folder\stem\PAIR.JPG` …）的文件不再被识别，也不会被 GC 删除。

## 测试
```bash
sh live-playback-ui/tests/test_preview_sync.sh
```
两组 host 测试（路径、清单、删除/保护、媒体/错误保护、原生适配层、开机 GC 普查）都已更新到新布局。

## 构建（win-lab WSL）
```bash
cd ~/.local/share/nikon-z8-qemu/live-audio
python3 pbin/relive_pb.py pbflat/src/live-playback-ui build-w5f/live-fixed.emulation-only.main build-w5f/changed-ranges.json build-pbf --defs='{"fx_glyph":"0x401f0000","fx_memmove":"0x401f0100"}'
```
- `relive_pb.py`（本目录）取代原来的 `build.py`：原脚本依赖旧机器上的基础镜像和 bridge64 core 构建目录，现在所有输入都取自当前镜像及其 `changed-ranges.json`（hook 的原始指令取自 range 的 `expected`）。
- `fx_glyph` / `fx_memmove` 在回放层里没有调用者（已对照部署版本的 `bl` 目标确认），所以给占位地址即可。
- **不能逐字节复现**：部署版本是 30,712 B，当时用的是另一版编译器；同一份源码用 clang 21 编出来是 30,168 B，平铺版是 30,104 B。区域大小保持 30,712 B（尾部补零）。
- 构建链：`relive_w5.py`（Live payload）→ `relive_pb.py`（回放层）→ `usb_default_on.py`。
