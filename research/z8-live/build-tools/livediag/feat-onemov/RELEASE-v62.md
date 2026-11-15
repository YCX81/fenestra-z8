# V62 发布源码检查点（2026-11-15）

用户已确认 V62 实际验证完成、编译与测试完成。本次按该确认整理提交，不再重复编译、运行测试、QEMU 或操作相机/存储卡。原始收据中的 `physical_camera_verified=false`、历史“实机待验收”描述保留交付时的证据范围，后续用户确认记录见 `v62/user-validation-20261115.json`。

## 功能与使用

- 修复视频 Log 行、子菜单标题与 N-Log / BT.2020、S-Log3 / S-Gamut3.Cine、Log3G10 / REDWideGamutRGB 文字。打开并退出 Modules 后，原厂文字仍按当前菜单上下文解析。
- 恢复 Live 快门声 On/Off，覆盖原厂有限音型及 Type B 单段音，修复中途切 Off 后麦克风不能恢复；原厂 Silent 仍优先生效。
- 保留 V61 的 RAW 优先 Live 标志、REC 播放与返回原 RAW，支持原厂 DCF 精确 JPEG 成员及跨卡副本配对；首次 MAN 不可用会限频重试。用户此前已确认双卡 RAW/JPEG 场景的标志、播放与返回正常。
- 保留 V60 的冻结剪帧规则、拍摄与 MOV 写入。保存忙碌时拍照可能没有新 Live，旧片段不会自动修复。

使用匹配集成固件，Live Ready 后拍照，等待保存；回放照片按 REC 播放对应 Live，结束返回原照片。保留现有 FENESTRA、DCIM、LIVE、LIVEDATA。内部身份仍为 `04.06.p0`，以版本目录和 SHA256 区分。

## Windows / WSL 构建来源

真实构建、链接、固件合成及整机 QEMU 在 Windows 实验室的 WSL Linux 环境运行，不是在 macOS 原生环境执行。Mac `/Users/…/Nikon` 是归档、交付和存储卡操作副本；脚本的 `/home/…` 路径属于 WSL Linux，不是缺失的 Mac 路径。发布时工作站用户名替换为 `/path/to/user`。

V62 的 `build_candidate.py` 通过 SSH 调用 `win-lab` 上的 `wsl -e bash -s`。最终 `main.json` 记录的实际构建目录是 `/path/to/user/fxfix/onemov/v62-log-shutter-8ec4cb/build`。历史前置包括 WSL 的 `v51src`、`v37build/v36`、`fxfix/v53-root`、`fxfix/v52-root` 和独立 QEMU 工具链。使用 LLVM 21.1.8；Python 依赖与私有原厂输入按原脚本及收据配置。这是现有实验室工作流的源码发布，未将其改写为 Mac 或全新机器的一键构建。

## 源码与前置提交

- `v62/work/` 是最终 Live/PB 源码；相对 `v61/work/` 仅快门 C 文件和 `hooks.S` 两个生产文件变化。
- `v62/shutter/patch_shutter.py` 保存源代码修改；`v62/menu/patch_menu_context.py` 与 fallback helper 保存独立、受限的菜单补丁。
- `v62/build_candidate.py`、`finalize_candidate.py`、`finalize_verification.py`、`pack62.py` 保存原构建、证据绑定与打包步骤。本次不执行这些脚本。
- `v62/final/` 保存已有最终审计与交付收据；`v62/menu/`、`shutter/`、`verification/`、`playback/` 保存对应工具和精选证据。MAIN、ELF、原厂 PCM、照片、卡镜像、原始跟踪及 SD UUID 不公开。
- V60、V61 是分批提交的前置历史；[V60 说明](RELEASE-v60.md)不是当前版本入口。旧诊断中的“尚未确认”按当时阶段阅读；后续 RAW 配对修复及用户反馈见 `v61/RESULT.md`。

## 既有验证记录与限制

已有记录包括：114164 项菜单 ARM 断言、122 组快门恢复 ARM、23 组原厂 SoundTask、104 组模型测试；7545 个二进制变化字节全部解释。V62 整个 PB bank/ELF 与 V61 相同，freeze 的 552 B 机器码相同。具体范围见 [V62 结果](v62/RESULT.md)和[最终二进制审计](v62/final/BINARY-AUDIT.md)。这些数字来自既有记录，本次未重复执行。

开启快门声需交接共用音频硬件，既有 Type B 模型测到约 0.257 秒录音静音。历史 On-r2 模型未输出最后一个源帧，Off-r4 覆盖全部源帧；记录仍保留 `all_observations_passed=false`，不能因用户总体实机确认而把历史失败改为通过。用户确认未逐项说明这些模型观察，不据此推断其全部消失。

MAIN SHA256：`36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5`。更新包 SHA256：`ddb949630186af582d51993286ff468383292800ed53571a2147a95c46853d89`。固件字节不随源码上传。

## 工作树清理

已合并的 Mac `fenestra-z8-v37` 与 Windows/WSL `v37cs` 工作树清理前均备份；Mac 保存 9 个改动文件及补丁，WSL 保存完整 6345 文件副本及补丁。备份分别位于工作区外的 `worktree-backups/v37-before-removal-20261115` 和 WSL 的 `worktree-backups/v37cs-before-removal-20261115`。仍供构建使用的 WSL `v51src`、未合并 portrait 分支和 QEMU 实验树保留。
