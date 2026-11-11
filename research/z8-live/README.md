# Z8 Live 开发进展快照（2026-11-11）

本目录已合入默认分支 `z8-c311`，按功能保存本地开发源码，不将实验实现直接合入仓库根目录的平台代码。它包含多个历史版本及互相替代的实验；目录存在不表示该版本通过验收。当前候选入口为 [v36 说明](build-tools/livediag/feat-release/RELEASE-v36.md)，部署前应结合后续硬件记录重新核验。

完整功能、操作流程、文件位置、构建依赖及离线检查方法见[Z8 功能与使用指南](../../docs/features-and-usage.zh-CN.md)。归档中的历史命令可能依赖未发布的实验室输入，先按该指南选择路线。

- `baseline-core/`：迁移包中保留的 Fenestra 核心与 SDK 源码，保持独立于仓库根目录当前实现。
- `build-tools/pb/`、`build-tools/w5/`：Live/回放及 Wave5 集成基础；根目录脚本保存打包与诊断配套工具。
- `build-tools/livediag/codex-w5/`：Wave5 生命周期、REC 交还、内存分配及历史版本分析。
- `build-tools/livediag/feat-rec/`：REC 记录器、灯码诊断、缓存锁迁移相关版本与设计记录。
- `build-tools/livediag/feat-icon/`、`feat-mov/`、`feat-c64label/`：原创图标、MOV 兼容性和 C64 标签实验。
- `build-tools/livediag/feat-release/`：v32 输入源码、v36 候选源码及构建/检查脚本。
- `audio-probe/`：音频分析脚本和说明；私人录音不随源码发布。

本次发布不运行 USB、相机、刷写、QEMU 或打包脚本。672 个导出的 Python 文件（两个仓库合计）通过离线语法解析；这不代表固件编译、链接、仿真或真机回归通过。历史文档中的测试结果与限制按当时上下文阅读，不能作为本次验证结果。

这是源码归档，不是可直接运行的完整实验室副本。固件、MAIN、转储、原始运行日志、影像素材、虚拟环境、编译产物、生成反汇编、原始助手会话及冗余生成 diff 保留本地。文档中的本地产物链接可能无法在 GitHub 打开；复现依赖需由使用者自行提供。工作站绝对路径替换为 `/path/to/Nikon` 或 `/path/to/user` 占位路径，复现时需配置本机路径。

[导出清单](export-manifest.json) 保存相对来源路径、原文件 SHA-256 与发布文件 SHA-256；仓库根目录 `SOURCE-INVENTORY.json` 校验发布字节。原始工作目录保持不变。项目材料沿用根目录 LICENSE、EXCEPTIONS.md 和 LICENSE-Z8.md；`minih264e.h` 等依赖保留其自身许可。QEMU 实验位于 [YCX81/z8-qemu](https://github.com/YCX81/z8-qemu/tree/main/experiments/20261111)，适用其独立许可。

2026-11-11 已补齐 [v36 离线复现入口](build-tools/livediag/feat-release/REPRODUCE-v36.md)：编译适配器、显式输入链接、本地清单重建和完整 MAIN 链。重新生成的 MAIN 与原 v36 哈希一致，真机回归状态保持未验证。
