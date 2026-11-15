# V61 构建与离线验证准备

2026-11-15；本轮仅只读检查现有本地/远端源与产物，未构建、未启动 QEMU、未操作实体相机或卡。实现目标由主代理确认：原 RAW 媒体 ID/DCF 节点作为播放桥接锚点，验证副卡的 JPG/MAN/MOV 后重定向文件读取；播放结束恢复原 RAW ID，不改变用户的 RAW 优先回放设置。

## 已确认的基线与空间

- 远端主机 `win-lab`，通过 WSL 使用 `/path/to/user/fxfix/onemov`。
- V60 最终 MAIN：`om8/q/main.main`，SHA256 `641725cec49a702a9440e790a6495b6aef4fb0f7ef5a71b6acbda2719ba47d49`。
- 源：`om8src`；构建目录：`/path/to/user/v37build/onemov-om8`。主代理已准备本地 `v61/work`。
- `/path/to/user/v37build/onemov-om8/build.json`：Ubuntu clang 21.1.8；seed SHA256 `50f2a5b22217710c6f35faf1e4639ef675f2738184525be0190f36bcc42cb36b`。

| 区域 | 范围（末地址不含） | V60 已用 | 余量 |
|---|---|---:|---:|
| Playback | `0x40196000..0x4019f020` | 36136 B | 760 B |
| Live 主区 | `0x401e2000..0x401fd000` | 109928 B | 664 B |
| Live extra | `0x401fd218..0x401fe000` | 3548 B | 12 B |
| Shutter | `0x4019f020..0x4019ff40` | 3116 B | 756 B |

不能扩大 PB 越过 shutter。若实现超过 760 B，主代理计划删除自 V54 起无调用的 `pb_scene_frame`/`clamp`；应先检查最终符号引用，再核实节省量。保留链接区域和其他功能层。

## 隔离构建方法

使用全新私有目录，例如 `/path/to/user/fxfix/onemov/v61-raw-peer-<unique>/` 放源码、构建和组合输出；不覆盖 `om8src`、`om8`、`onemov-om8`。直接执行构建/组合工具，不使用有目录缓存及删除行为的 `go.sh`/`cap.sh`。

现有构建入口：`/path/to/user/fxfix/onemov/build_onemov.py`。它读取 `/path/to/user/fxfix/v53-root/build_v53.py` 并增添四个原厂 BL 调用点，继续使用 v36 seed、clang 21、分区链接。

```text
python3 -B /path/to/user/fxfix/onemov/build_onemov.py \
  --v36-dir /path/to/user/v37build/v36 \
  --source-dir <private-source> --output-dir <fresh-build>
python3 -B /path/to/user/fxfix/v53-root/compose53.py <fresh-build> <fresh-composed>
```

构建后验收：所有 size assert 通过；记录源文件、ELF、payload、最终 MAIN 哈希；逐区比较 V60。除本次 PB 代码及关联 BL/构建 ID 外，Live 帧数规则、writer、音频、快门、其他 hook 应保持预期字节。若组合过程造成额外变化，应逐项解释，不直接放行。现阶段不执行 pack60；交付版本需独立 V61 包装记录。

## 可用 fixtures 与 QEMU 的真实边界

- 真实 NEF：本地 `/path/to/Nikon/_mac-migration/firmware/_DSC0128.NEF`；远端 `/path/to/Nikon/z8-qemu/.local/firmware/_DSC0128.NEF`。两者 SHA256 `d519a7bb2b7365a9ede204718d8ab80e9d369829d9767d16a40ccdf5649bf3ee`，59237510 B；TIFF 内 Make=`NIKON CORPORATION`、Model=`NIKON Z 8`、Software=`Ver.03.01`。它是独立历史样本，不能声称与本次 9085 等照片属于同一快门事件。
- 现有可用 QEMU JPG/MOV/MAN：`/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/om8-capture/files/` 下的 `DCIM/100NCZ_8/DSC_0001.JPG`、`LIVE/100NCZ_8/DSC_0001.MOV`、`LIVEDATA/100NCZ_8/DSC_0001.MAN`。
- 现有回放卡：`/path/to/user/fxfix/onemov/pom8f-card.raw`、`pom8n-card.raw`；须创建私有副本后使用，现有卡可能已经被旧测试写入。
- 本地新卡 MOV：`../v60-diagnosis/card-20261115/LIVE/108NCZ_8/DSC_9085.MOV`（93 帧）、9089/9090（90 帧）；全为 1024×682 HEVC，30000/1001 CFR，元数据/解码已验证。用于媒体回归时应保留真实源文件哈希，测试重命名和 MAN 重建应另行标明。

现有 `/path/to/Nikon/rdwing-z8-qemu/tools/run_native.py` 仅暴露一个 `--card`；`tools/virtual_card.py:attach` 固定添加一个 `nvme,drive=card`，原生 MCU runner 还启用 `zr-sd-empty-fixture=on`。本次检查未发现第二卡 CLI 或 SD 存储后端。因此不能直接承诺“真实双卡 RAW→副卡 Live”全系统 QEMU 验收。可以执行普通 JPG/单卡 NEF 回归；跨 A/B 路径与返回 RAW 契约应使用下面的最终 AArch64 PB 隔离测试覆盖。若追加模型开发，应作为独立工作，不能以单 NVMe 冒充双卡。

## 偏移与观测地址必须重新生成

旧 `driver-om8/pb-offsets.json` 与 `pb-diag-layout.s` 仍写 `struct_bytes=9352`，而 V60 实际运行 `PbRuntime.bytes=9632`。已有前缀字段未必偏移变化，但该旧表不含 one-MOV 新状态，不能直接复制作为 V61 ABI 证明。旧只读插件的硬编码 UI 采样地址也来自更早构建。

生成方式：针对最终 V61 `pb-fix37/runtime.h` 编译常量数组，首项 `sizeof(PbRuntime)`，随后每个需要观测字段的 `offsetof` 和 `sizeof`；使用 `clang-21 --target=aarch64-none-elf -c -I<final-pb-source>`，从 ELF `.rodata` 读取小端 u32。现有范例是本地 `feat-v53/audio/core-f/prepare.py` 的第 9–15 行。不要运行整个 prepare.py，它还会修改旧驱动。

字段至少包括：`current_id/cached_id/cache_valid/lookup_count/lookup_error/native_path/manifest`，`nb_photo/nb_movie/nb_stage/nb_from/nb_to/nb_bit/nb_fail/nb_starts/nb_returns/nb_cleanups/nb_w/nb_h`，`consumed_rec/ui_leaving/key_epoch/media_epoch/shutdown`，以及本次新增的 RAW 源路径或候选路径状态。使用最终 `pb/payload.elf` 与 `pb/changed-ranges.json` 获取地址；调用点内部偏移由最终反汇编核实，不能沿用旧绝对地址。

## 验证矩阵

| 场景 | 推荐层级 | 必须观察的结果 |
|---|---|---|
| 原 JPG 正常 Live | 最终 PB 契约测试 + QEMU | 标志出现；REC 仅消费一次；重定向正确；结束回原 JPG；重复播放可行 |
| A 卡 RAW，B 卡对应 JPG/MAN/MOV | 最终 AArch64 PB + 文件 API stub | `current_id`/DCF 锚点始终是原 RAW；验证 B JPG 的实际长度与 MAN；读取 B 的 LIVE MOV；结束写回原 RAW ID |
| RAW 自身已有合法 Live 配对 | 契约测试 | 保留原合法 RAW 配对路径；不错误优先选择同名副卡 JPG |
| 同卡 NEF→对应 JPG 配对 | 契约测试；可行时单卡 QEMU | 原 RAW ID 保留；副本路径和返回 ID 解耦；NEF 显示能力须实际确认 |
| 另一卡有同号但不匹配 MAN/JPG | 契约测试 | 路径、文件大小或身份验证不符时不显示可播/不拦截 REC；不盲播同号 MOV |
| MAN 缺失、损坏、JPG 大小不符、MOV 缺失/非法 | 契约测试 | 失败状态可解释；没有临时 DCF bit 残留；用户仍可正常离开回放 |
| 拍摄后第一次查询早于 MAN 提交 | 受控文件可见性测试 | 文件后来有效时能重新识别（若本次实现包含重试）；不以切换图片掩盖首次负缓存 |
| 播放正常结束、提前退出、切图、离开回放 | 契约测试 + QEMU 可达路径 | 正确还原原 RAW ID/原节点 mask；不把副卡 JPG 当返回目标；不对其他节点清 bit |
| 连播、快速重复 REC、播放清理窗口再按 REC | 契约测试 + QEMU | 一致的 stage/ownership；无双重启动、错误节点、永久 ui_leaving |
| 普通非 Live JPG/NEF、普通 MOV | 契约测试 + QEMU | 原键处理和普通播放保留；无跨卡猜配后错误截获 |

AArch64 隔离测试应装载最终 PB ELF/段，运行真实编译后的 `lookup`、`nb_start`、open/stat wrapper、return/cleanup 路径；仅将原厂文件服务、DCF 锁/节点定位及页面切换作为记录参数的 stub。A/B 文件映射到不同私有 host fixture 根目录，明确这证明代码契约而非真实 SD/CFexpress 服务和线程调度。

QEMU 需使用单独运行目录、私有 card 副本、最终 MAIN 精确 SHA。不要把“拍摄成功”计作识别入口成功：V60 `om8-capture` 在 `--mode on` 时 `pb.lookup_count=0`；必须观察实际 lookup、图标和 REC 入口。旧 `--mode pb` 驱动期望 JPEG 自带播放器 `frames` 字段，也需按 one-MOV 的 `nb_stage/nb_starts/nb_returns` 与页面 `0x56c→0x552` 改验收。

最终报告分开写：静态与契约测试、QEMU 可达场景、真实双卡/真实相机仍未验证的部分。任何层级通过都不替代另一个层级。
