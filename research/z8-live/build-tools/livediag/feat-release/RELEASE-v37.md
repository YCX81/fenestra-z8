# Z8 Live v37（2026-11-11，候选，未上真机）

> **上传快照更新（2026-11-11）：** 当前源码还包含后续加入的机内预览音频路径，以及把预览解析/解码移到 Live 层的空间调整。下文是此前无声版本的记录，其中哈希、空间余量和测试次数不代表最新源码。当前构建与验证范围见 [CHECKPOINT-v37.md](CHECKPOINT-v37.md)。音频播放尚未得到有效运行或真机验证。

v37 = v36 + 三处改动，全部在两层 Live 代码里；核心、NFP、C64、USB、GUI pool、bootguard 与 v36 逐字节相同（同一 seed）。

| | v36 | v37 |
|---|---|---|
| 每张 Live 写入的视频 | `LIVE/…/DSC_nnnn.MOV` + `DCIM/…/LIV_nnnn.MOV` 副本 + `LIVEDATA/…/DSC_nnnn.LVP`（约 47 MB 原始预览） | 只有 `LIVE/…/DSC_nnnn.MOV` |
| 机内预览来源 | 独立 LVP 文件 | MOV 自身尾部的 `free` 原子（45 张 1024×682 JPEG） |
| LIVE 图标/角标 | 只在状态变化时绘制 | 定时重绘，连续失败后摘掉重挂 |

配对 JPG（`LIVE/…/DSC_nnnn.JPG`）和小清单（`LIVEDATA/…/DSC_nnnn.MAN`，约 110 字节）保留。

## 1. 只保存一个 Live MOV

- `codec/preview_writer.[ch]` 重写：基线 JPEG 编码器（整数运算、无分配、无可写静态数据），预览帧在 `moov` 之后写成一个 QuickTime `free` 原子。任何 MOV 读取器都会跳过它；`ftyp/mdat/moov` 与 v36 相同。布局见头文件注释，文件最后 8 字节是 `长度 + "LVJT"`，读取端由此回溯。
- 质量从 45 起；某帧超过 72 KiB 后续帧降一档（高噪声场景总量约减半），单帧超过 192 KiB 时当帧降档重编；中途失败按原事务规则删除整个 MOV，不会留下半个原子。
- `codec/live_transaction.c`：不再创建 LVP；预览写在 MOV 关闭之前。
- `capture/live_stream.c`：`dcim_copy` 默认 0（不再往 DCIM 复制第二份 MOV）。
- 体积：测试图样每帧约 16–28 KB（45 帧 0.7–1.3 MB），高噪声最坏情况每帧约 130 KB。真机实拍体积待测。
- 没改的：RAM 里的 50 帧预览环（约 52 MB 池）仍在，只是不再原样写卡。

## 2. 机内预览

- `pb-fix37/preview.[ch]`：`free` 原子解析 + 基线 JPEG 解码器（查表哈夫曼、整数 IDCT、仅 DC 块走快路径）。
- `pb-fix37/runtime.c`：REC 先找 MOV 里的预览，没有再找旧 LVP，所以 v36 及更早拍的 Live 仍能播。解码失败只停止本次播放。
- 仍是无声预览，不是用原厂播放器解 H.264。原厂播放路线（`feat-mov/firmware-plan.md`）的阻塞项没有变化。

## 3. 图标

- 拍摄画面：`tune.icon_refresh_ms` 默认 400（原为 0），原生界面重画把图标盖掉后最迟 0.4 s 重绘；连续 6 次呈现失败则摘掉旧挂载重新挂（`icon_heals` 计数）。
- 回放角标：按任意键后原来要换一张照片才回来，现在按键静止 0.7 s 后自动回来；并每 0.5 s 重绘一次。REC 没东西可播时角标立即恢复。

## 构建

```sh
bash tools-v37/make.sh /path/to/v36-from-stock-output /path/to/new-v37-output
```

需要已完成的 `from-stock/build.py` 输出目录（取其中 `seed.main`、`seed-manifest.json`）。2026-11-11 本机结果：`v37.main` SHA-256 `e52b7154eb4999dbb61c7be7fb6345aab1fc3f05457b7571d545926191ed00c8`；Live payload 85,128 B（余 29,560），回放 payload 39,576 B（余 1,384）。`patch_live.py`、`patch_pb.py` 记录了相对 v36 源码的精确替换。

未生成更新容器：`from-stock/nfp_backend.py pack` 固定校验 v36 的 MAIN 哈希，本次没有改它。

## 验证

主机（`tests-v37/run_preview.sh`，ASan/UBSan）：编码 → 原子 → 解析 → 解码往返；本解码器与 ffmpeg 解同一批 JPEG 亮度最大相差 1；任意位置写卡失败都返回错误。

QEMU（`tests-v37/qemu/`，本机 11-04 的 merged QEMU）：

- 本机 QEMU 的 Wave5 模型完成不了 v32+ Live 用的照片档 bring-up，**未改动的 v36 同样每帧分配重试、永远不 Ready**。所以真正的 `v37.main` 在 QEMU 里只验证到：启动、两层加载、菜单开 Live、图标显示、进出菜单/回放。
- 完整流程用仅供仿真的 `build_swenc.sh` 镜像验证：把 `codec/live_w5enc.c` 换成树里保留的软件编码器 `codec/live_encoder.c`，其余单元就是 v37 源码。三次并行运行结果一致：1 次事务成功；卡上只有 `LIVE/…JPG`、`LIVE/…MOV`、`LIVEDATA/…MAN`；`check_mov.py` 通过（原子顺序 `ftyp mdat moov free`，ffmpeg 全片解码无错，45 张预览）；回放 REC 被消费、播 45 帧、解码错误 0，再按 REC 重播到 90 帧；按键后角标摘下再回来；从菜单、回放返回后拍摄图标重新挂上。
- 共 6 次 v37 完整场景全部走完；同场景的 v36 对照（`build_swenc.sh … src-fix36 pb-fix36`）3 次里 2 次出现固件自己的 “Error. Turn camera off and then on again”（取景停止 / 退出菜单后不重启），v37 的旋转、切录像档 3 次运行也都撞上它，所以这两个场景在 QEMU 里没有有效结果。该报错是本机 QEMU 的既有偶发问题，与版本无关。
- 完整场景的运行里 `icon_heals` 始终为 0：QEMU 没有复现真机上的图标丢失，因此图标修复是按代码路径推断的，是否解决真机现象要上机看。

没有验证：真机上的任何行为；Wave5 码流与新写入路径的组合；JPEG 编码在相机 CPU 上增加的保存耗时；解码能否跟上 15 fps（跟不上时按时间丢帧）；Mac/iPhone 是否仍把带尾部 `free` 的 MOV 识别为实况。
