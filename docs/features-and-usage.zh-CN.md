# Z8 功能与使用指南

更新日期：2026-11-11。仓库默认分支为 `z8-c311`。Live 开发进展已合并，原临时进展分支已清理。

## 选择使用入口

本仓库提供两套需要区分的源码：根目录是 Fenestra Z8 C3.11 核心移植；`research/z8-live/` 是后续 Live 与回放开发归档，包含基线、候选版、诊断版和历史失败方案。合并归档不改变根目录构建器的功能，也不会自动把 v36 加入核心构建。

| 功能 | 入口与使用方式 | 当前状态 |
|---|---|---|
| Fenestra 菜单与激活 | Setup → Fenestra；Activate、Auto activate、Shutdown guard、Modules；WonderLantern 打开模块页 | 根目录核心有 QEMU 菜单/激活记录；自动激活、真实断电和实体 USB 的覆盖范围见[移植记录](../port-z8/PORT-STATUS.md) |
| 独立模块与显示 | 构建目标为 `z8-c311` 的 `.wlm`；hello 菜单示例、demo 字符雨、SDK 和原生显示适配 | 有 QEMU 运行记录；根目录核心未作真机验收 |
| Linux INFO 桥与主机调试 | 核心构建生成匹配的 INFO agent；入口为 `host/ptp_linux.py`，详见[调试说明](debugging.md) | 仅无参数 INFO；共享内存软件回环有记录，实体 USB/PTP 未验；不提供 Z8 shell/任意命令执行 |
| Live 拍摄开关与状态 | 集成版中 Setup → Fenestra → Live Photo → On / Off；界面报告 Off、Requested、Ready、Busy、Unavailable 或 card error | 在后续 Live 源码中；v36 是候选，不能把源码存在视为完整真机通过 |
| 快门前后视频与照片配对 | Live Ready 后正常拍照；Wave5 H.264、MOV 时间戳与静态照片标记；目标窗口为快门前后各 1.5 秒 | 窗口可受启动时间、关键帧、丢帧影响；不是 RAW 预拍，不保证每张固定三秒 |
| Live 音频 | 同步音频采集、与短片窗口配对、MOV 音轨及增益处理 | 采集失败可退回无声短片；v36 音量、同步与连续保存仍需回归 |
| Live 照片回放 | 单张照片回放页显示 LIVE 徽标，默认以 REC 触发 LVP 预览；另有 REC / Off 键设置源码 | LVP 预览和原厂 MOV 播放是两条路径；菜单是否出现取决于 PB 构建宏，见下文 |
| 横竖持与 LIVE 图标 | 拍照静态/忙碌扫光徽标；回放静态徽标；按显示目标、viewport 和方向刷新 | v36 素材为 110×44；LCD/EVF 实际表现和切屏时序待按版本核验 |
| Wave5 交还与 REC 兼容改动 | v36 将关闭锁移到缓存 runtime，保留原照片/录像切档及 bring-up 交还策略 | 离线源码/对象审计有历史记录；v9b 的用户硬件报告不等于 v36 硬件回归通过 |
| C64 标签字号 | 独立补丁把既有 C10…C64 共享描述符的样式改为 28 号，见下文 | 只调整已扩容输入的标签，不创建 C64 槽位；会影响共用标签的多个页面，完整显示仍需验证 |
| REC 诊断与分析工具 | `feat-rec/` 保存记录器、灯码、解码器、检查脚本；`codex-w5/` 保存生命周期分析 | 属于指定诊断版，非普通拍照开关；v36 未引入 v9b recorder/lamp，不应混用工具或结构偏移 |

尚未提供可从本仓库直接下载、安装并完成真机验收的 Z8 发布包。没有把 iPhone 导入、原厂播放器接受 Live MOV、全部 RAW+JPEG/双卡组合或连续 REC 往返列为已完成能力。根目录保留的 ZR/Z9 工具和上游 Releases 也不是 Z8 下载入口。

## 获取源码与依赖

```sh
git clone --branch z8-c311 https://github.com/YCX81/fenestra-z8.git
cd fenestra-z8
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
```

构建需要 LLVM 的 `clang`、`ld.lld`、`llvm-objcopy`、`llvm-nm`。Z8 核心还需可静态链接的 AArch64 Linux GCC，默认命令为 `aarch64-linux-gnu-gcc`，用于 INFO agent；可用 `--agent-cc` 指定兼容编译器。按实际系统安装工具链，不混用宿主可执行文件与 AArch64 目标文件。USB 主机工具的附加依赖见 `requirements-host.txt`，安装依赖不等于需要连接相机。

原始固件及 MAIN、密钥、匹配 manifest、生成镜像和实验介质由使用者自行提供。放在本地 `inputs/`、`private/`、`build/` 或仓库外，不能提交。后续 Live 归档中 `/path/to/Nikon`、`/path/to/user` 是脱敏占位路径，需按实验室布局配置；它们不是可直接复制使用的实际路径。

## 构建核心与示例模块

以下命令在仓库根目录执行；输出目录使用新路径：

```sh
python host/build_z8_core.py \
  --firmware /absolute/path/to/z8-c311-original-main.bin \
  --output-dir build/z8-core-local

python host/build_sdk_module.py examples/hello/module.c \
  --id hello --target z8-c311 --output-dir build/hello-z8-local
python host/build_sdk_module.py modules/demo/sdk.c \
  --id demo --target z8-c311 --output-dir build/demo-z8-local
```

核心输入为身份匹配的完整原始 MAIN，不是加密升级容器，也不是已经打过 Live 补丁的 MAIN；构建器校验输入身份。输出是仿真候选，不能当作可安装的相机升级文件。通用 `fenestra.py make-patch` / ZR、Z9 发布补丁不适用于本 Z8 路线。

在配套 QEMU 实验的虚拟卡上，将构建出的兼容 `.wlm` 放到 `FENESTRA/`；启动后在 Setup → Fenestra 激活，在 Modules 查看模块，在 WonderLantern 打开模块页。Disabled 文件使用 `.wlm.disabled`；已经加载的模块不能因重命名就视为卸载，通常需重启。异常关机 flag 会阻止加载；Clear abnormal flag 的历史测试范围见移植记录。

`port-z8/run-boot.sh` / `fenestra_boot.py` 是依赖独立研究环境的启动器，需 `FENESTRA_NIKON_WORKSPACE` 等配置；单独克隆本仓库并不能复现整套实验。QEMU 的基础构建及运行入口见[配套使用指南](https://github.com/YCX81/z8-qemu/blob/main/docs/features-and-usage.zh-CN.md)。

## 在匹配的 Live 集成版中操作

以下描述的是归档代码中的交互契约，前提是已经有匹配的完整集成构建；仅构建上述根目录核心不会出现这些功能。

1. 进入照片模式和取景，打开 Setup → Fenestra → Live Photo，选 On。Requested 表示已请求启用，Ready 才表示当前捕获状态就绪；Busy 期间等待处理结束。card error 表示配置写卡失败，Unavailable 表示当前捕获/任务出错。
2. Ready 后正常按快门。Live 处理依赖原生 still writer 完成及文件身份，不另行分配照片编号。前后窗口不足、保存忙碌或状态切换时可能短片变短、请求被忽略或取消。
3. 忙碌扫光结束后核对原照片及其 Live 文件。音频不可用时短片可能无声，不能仅以存在 MOV 判断音轨正常。
4. 进入该照片的单张回放页，匹配的 `.MAN` / `.LVP` 可用时显示 LIVE 徽标。默认 REC 启动预览；播放中再次按 REC、离开照片或其他取消按键会使预览停止并恢复原厂界面。非 Live 照片沿用原厂按键处理。
5. `pb-fix36/settings.c` 实现播放键 REC / Off 设置；定义 `PB_SETTINGS_DISABLED` 的组合构建不安装该菜单，播放键保留默认 REC。不要假设每个历史版本都有该设置项。照片回放中的 REC 预览与录像档的 REC 录制不是同一操作。
6. 选 Off 停止 Live 请求。照片→录像→原厂 MOV 回放→照片的完整往返属于 v36 待回归项，不能仅凭早期诊断版记录宣称全部兼容。

代码按原照片路径派生以下文件（以 `100NCZ_8/DSC_0001.JPG` 为例）：

| 相对卡根目录路径 | 作用 |
|---|---|
| `DCIM/100NCZ_8/DSC_0001.JPG` | 原厂照片，由原生 writer 管理 |
| `LIVE/100NCZ_8/DSC_0001.JPG`、`.MOV` | 配对照片副本与短片；配对标记写入副本，保留原照片 |
| `LIVEDATA/100NCZ_8/DSC_0001.MAN`、`.LVP` | 身份/长度等配对元数据与机内预览数据；不是通用视频格式 |
| `DCIM/100NCZ_8/LIV_0001.MOV` | 启用且成功完成 DCIM 复制时的短片副本，便于原厂 MTP 枚举；不保证原厂播放器接受 |
| `FENESTRA/live-z8.0`、`live-z8.1` | 双副本 Live 开关配置；不要当作普通文本编辑 |

导出时保留对应照片与 MOV。只导出 LVP 无法作为普通视频使用；DCIM 出现短片也不等于系统相册已将其识别为 Live Photo。历史版本使用过不同命名和编码路径，应以该版本源码为准。

## v36 开发与离线检查

当前候选目录为 [`research/z8-live/build-tools/livediag/feat-release/`](../research/z8-live/build-tools/livediag/feat-release/)，Live 输入为 `src-fix36/`，回放输入为 `pb-fix36/`。它保留了 v32 Live、音频和竖屏逻辑，迁移 Wave5 关闭锁，并应用 110×44 的原创透明徽标。图标旧说明里的 112×44 / 中线 49 不代表 v36；v36 生成配置为拍照中线 54、y=98，回放 x=y=24，均需结合 viewport 解读。

下面只编译对象、比较 ABI 并检查原子指令，不打开 USB、运行 QEMU 或生成升级包：

```sh
# 从仓库根目录、已激活的虚拟环境开始；clang 需支持 aarch64-none-elf。
python -m pip install pyelftools capstone
cd research/z8-live/build-tools/livediag/feat-release
python -B tools-v36/compile.py
python -B tools-v36/layout.py
python -B tools-v36/audit.py
```

输出在本地 `audit-v36/`。历史构建使用 clang 21，28 个 shipping 编译单元；更换编译器需重新核对结果。发布归档未携带历史对象和审计产物，因此文档里的部分本地产物链接不存在。

完整链接另需对应的私有参考 MAIN、符号/范围 manifest 和 ELF linker。现已补齐 clang 适配器、可移植 chain 入口与本地清单重建工具；`link.py` 支持显式输入路径。2026-11-11 重跑了 28 个单元编译、布局、194 条独占指令审计、ELF 链接和完整 MAIN 链，最终 MAIN 与原 v36 逐字节一致。步骤和固定输入见[复现说明](../research/z8-live/build-tools/livediag/feat-release/REPRODUCE-v36.md)。`check_icons.py` 仍依赖未发布的旧测试 fixtures；本次没有重跑图标 fixture、QEMU 或真机回归。正常复编无需运行 `prepare.py`。原始[候选说明](../research/z8-live/build-tools/livediag/feat-release/RELEASE-v36.md)保留原交付时的记录。

## C64 标签补丁

先准备与脚本固定 SHA-256 匹配的私有 `bg34.main` 参考，以及相同布局的待处理 MAIN；输出与 JSON 路径须不存在。在仓库根目录执行：

```sh
python research/z8-live/build-tools/livediag/feat-c64label/c64label_patch.py \
  /absolute/path/to/input.main /absolute/path/to/new-c64label.main \
  --reference /absolute/path/to/bg34.main \
  --json /absolute/path/to/new-c64label.changed.json
```

脚本只改 275 个描述符的单字节样式，遇到错误参考、未知布局或重复补丁会拒绝。它不是原始 C3.11→C64 的扩容器，也不是固件打包器；没有证明 28 号字能在每个页面完整显示，更不能自动应用到 v36。原理和共享影响见[C64 结果](../research/z8-live/build-tools/livediag/feat-c64label/C64LABEL-RESULT.md)。

## 验证与问题报告

先运行 `python -B verify_source.py` 校验源码清单。复现问题时记录仓库提交、精确版本目录、MAIN/manifest 哈希、构建命令、菜单/按键序列和错误计数。分别写清“源码推断”“历史仿真记录”“该版本真机观察”；只报告必要的数字与身份，不上传固件、密钥、转储或私人素材。

本次更新只维护文档和分支，不新增硬件、USB、QEMU 或导入兼容性验收。许可适用范围见 [LICENSE-Z8.md](../LICENSE-Z8.md) 与 [EXCEPTIONS.md](../EXCEPTIONS.md)，独立依赖保留原许可。
