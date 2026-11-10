# Plan A v4：实现与仿真批次完成，双拍验收失败

**v4 rerun 更新结论：默认已改为 speed 9（deblock 开），完整 port 配置下两次单拍与两次 Off 均通过；双拍仍被 QEMU 第二张 thumbnail／source handoff 的 strict fault 阻断，未满足两张原生照片保存。以下保留上一轮结果，复跑证据见文末。**

已读 v3 报告，在现有 v3 副本上完成实现、主机／静态检查、一次正式批次及唯一重试批次。**两次成功写出单张 Live 配对，但未满足“两张原生照片均保存”，不能宣称整体验收通过。**

实现见 [stream](capture/live_stream.c)、[runtime](runtime.c)、[编码接口](codec/live_encoder.c)。CPU2 worker 使用 processor=3、affinity=4、priority=30；另一个 priority=31 的 CPU2 worker 在原生 ticket 的 success、closed、size 均成立后 mux。NV12 按运行时尺寸逐行转 I420，不缩放；minih264 `const_input_flag=1` 处理宏块边界。主机验证 1024×680 → 内部 1024×688、SPS 裁底 8 行；本次 QEMU 经菜单后 **B1 本身为 1024×682**，输出随实际几何为 1024×682，不强改为 680。

默认目标 8 Mbit/s、按 PTS 每 ≥0.5 s 发 SPS/PPS＋IDR；[build.py](build.py) 提供 `--bitrate`、`--idr-us`。5 MiB 压缩历史 ring 可容纳 8 Mbit/s 的 5.24 s，另有独立 5 MiB 冻结区，防止 mux 时历史淘汰破坏片段。按下时选不晚于 press−1.5 s 的最后 IDR，保存至 press＋1.5 s；冻结后继续编码。只允许一个待处理 Live，**后卷及 mux 期间的后续 Live 请求均忽略**，原生按键仍直接转发。队列为容量 1 的最新帧邮箱，跨过的完成事件计入丢帧，IRQ 不复制／编码／等待。状态块包含捕获、编码、丢帧、guest 编码 avg/max、有效 fps、ring 用量及 mux 前后编码数。

[主机完整尺寸测试](stream-test.txt) 通过：77 帧 MOV 可解码、每帧不同；覆盖带 stride 的 NV12、运行时换尺寸、积压丢帧、滚动区全部淘汰后冻结区仍可 mux、取消等待消费者 ACK，以及持续 5 fps 告警。[原始帧](codec/transaction-results.txt)与[预编码 AU](codec/transaction-au-results.txt)两条事务路径均通过 41 处 I/O 故障、15 处短传输、15 处错误 seek，以及取消／冲突／清理测试；[codec 全套](codec/check-v4.txt)、[capture](capture/check-results-v4.txt)、[64 个 AArch64 hook 用例](capture/v4-hook-check.txt)、[ticket](capture/v4-ticket-check.txt)通过。复现入口：[check-v4.sh](check-v4.sh)。freestanding overlay 构建、无可写静态区／修改范围检查通过；新增配置参数后重建与实跑镜像逐字节相同，见 [build-summary](build-summary.json)，镜像 SHA-256 为 `628b1c05af015e8b8b47dc1a9b5b004eb9337fbc24827f7c00a89731726983f3`。

实际批次：[首批](batch-v4-results.json)、[唯一重试](batch-v4-retry-results.json)、[数字审计](evidence-summary.json)。五次进程均 exit 0，**exit 0 不代表拍摄成功**。

| run | 卡上结果与截图 |
|---|---|
| `v4-on` | 原生 JPG＋PAIR.JPG＋CLIP.MOV＋MANIFEST；[截图](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-on/step-111-post-encode-30s.png)。菜单后 LV 曾停，半按后恢复，按下前实际编码约 1.8 s；不据此声称已验证 ≥5 s 稳定预卷。 |
| `v4-off` | 一张原生 8256×5504 JPG，无 LIVE 文件；全部三幅 MPO 解码像素等于 control，见[像素审计](v4-off-analysis.json)、[截图](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-off/step-052-post-complete.png)。 |
| `v4-two` | 两次全按间隔约 2.043 s；LV 在全按前停止，irq_seq 固定 466，编码 0、原生 JPG 0、Live 0；[截图](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-two/step-114-post-complete.png)。 |
| `v4-retry-on` | 增加半按／释放 warm-up 后仍停在 irq_seq=478；原生 JPG 0、Live 0；[截图](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-retry-on/step-114-post-complete.png)。 |
| `v4-retry-two` | 首按前持续编码 **19.039 s**；两次独立按下约隔 **2.016 s**，仅一张原生 JPG及一组 Live。第二 Live 请求发生在 mux 中而忽略；第二原生照片也未写出，writer_returns=1，JPEG 模型 jobs 在第二按后仍为 11；原因未定。[截图](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-retry-two/step-117-post-complete.png)、[完整审计](v4-retry-two-analysis.json)。 |

成功媒体：[原生 JPG](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-on/files/DCIM/100NCZ_8/DSC_0001.JPG)、[PAIR.JPG](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-on/files/LIVE/100NCZ_8/DSC_0001/PAIR.JPG)、[CLIP.MOV](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-on/files/LIVE/100NCZ_8/DSC_0001/CLIP.MOV)、[MANIFEST](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-on/files/LIVE/100NCZ_8/DSC_0001/MANIFEST)；另一组 [MOV](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4-retry-two/files/LIVE/100NCZ_8/DSC_0001/CLIP.MOV)。两组原生与 PAIR 各 1,169,905 B，三幅 MPO 像素完全相同。

| 成功 run | MOV：Baseline L3.1，1024×682 | still-image-time／配对证据 |
|---|---|---|
| `v4-on` | 78 帧，3.284243 s，152,920 B，视频 366,187 bit/s；78 个不同帧 hash | 1.761518 s；ID=`E31C995E-E0BA-1341-46B0-3B3A0D75AB68`；[ffprobe](v4-on-DSC_0001-ffprobe.json)、[ExifTool](v4-on-DSC_0001-exiftool.txt) |
| `v4-retry-two` | 78 帧，3.262752 s，145,676 B，视频 350,837 bit/s；78 个不同帧 hash | 1.761517 s；ID=`208729E9-C490-7091-BC5F-9CB4B3FF00B2`；[ffprobe](v4-retry-two-DSC_0001-ffprobe.json)、[ExifTool](v4-retry-two-DSC_0001-exiftool.txt) |

两片首帧均 IDR，IDR 间距约 0.508 s。`mebx` sample 均为 `0000000900000001ff`，PTS 落在包含按键时刻的视频帧；JPEG／MOV／MANIFEST 的 ID 匹配。低码率来自简单合成画面及 VBR，**不证明复杂场景可保持目标码率或画质**。

| run | 捕获／编码／丢弃 | 每帧 guest 平均／最大 | 有效 fps | ring 峰值 | mux 期间新增编码 |
|---|---:|---:|---:|---:|---:|
| `v4-on` | 1059／1059／1 | 23.051／44.744 ms | 23.624 | 478,086 B | 13 |
| `v4-retry-two` | 2234／2233／1（快照有 1 帧在途） | 23.002／44.888 ms | 23.616 | 480,595 B | 12 |

两次成功 run 的积压丢帧为 0，1 帧为源读取拒绝，未观测持续低于 15 fps。时间由 guest CNTPCT/CNTFRQ 包围编码调用取得，包含被抢占时间，不含 NV12 转换；不是 host 用时或真机 CPU 周期。没有采集可归属到单帧编码的 guest 指令数，不能把全机 icount 时间当作编码指令数。

RAM：[尺寸复算](memory-v4.txt)。1024×682 请求 **15,542,575 B**，含两份 5 MiB arena、I420 1,047,552 B、encoder persistent 2,282,096 B、scratch 1,641,823 B、事务 workspace 85,216 B 及对齐。沿用 v3 class 0x24，QEMU 实占 **22,904,832 B**；v3 320×212 常驻 ring 实占 11,452,416 B，因此常驻池占用反而增加 11,452,416 B，不能宣称比缩小版 v3 更省 RAM。v3 编码期另有分配，本轮未同条件测其总峰值。v4 runtime＋两个私有栈另请求 143,616 B；`v4-on` 关闭后池占用归零，主机测试验证释放。

硬件仅作假设估算：核心类型／频率未知，按编码栅格 1024×688、50–200 cycles/亮度像素、0.5–2 GHz，纯 CPU 约 **17.6–281.8 ms/帧**；若仅获得 25%–100% 调度时间，约 **17.6–1127.2 ms/帧**，另加转格式／内存竞争。这些参数未实测，不能用 QEMU 的约 23 ms 承诺真机实时性。

使用指定未重建 QEMU，SHA-256 见数字审计；`FX_ICOUNT_SLEEP=off`，ext4 运行目录、sparse 卡，复制使用 `cp --sparse=always`，按可见进程及可用内存调度，启动间隔均约 100 s；每次启动可用内存 ≥18 GiB。各 run 的大 raw/bin 已删除，日志、JSON、PNG、媒体保留；审计时 C: 可用约 231 GiB，始终高于 30 GiB。

未证明：两拍原生保存、LV 停止原因，以及 Live 是否干扰第二次 still；不能把这些失败直接归因于既知 0.3 s 双按模型缺口。best-effort snapshot／cache 仍不是真实 DMA lease；CPU2 NEON 上下文、硬件性能／热、复杂场景码率、曝光而非按键锚点、RAW+JPEG／双卡、跨重启卷身份、断电恢复、原生回放、iPhone 导入均未验收。状态 ring_fill 在关闭后的历史值应结合 actual_bytes=0 解读，当前底层 ring 已释放。未制作更新容器、刷机、重建共享 QEMU、commit 或 push。


## v4 rerun

本轮完成 **一批＋唯一一次重试**，每批五个指定用例及一个 speed-10 对照，共 12 次 QEMU；无额外实跑。使用指定 [qemu-port](../qemu-port/REPORT.md) binary，SHA-256 `2c43029b6a500860fd7f4d5184b4ea00965784f7ee78d7e05357a41fe4b68784`，未重建。见[首批](rerun/batch-results.json)、[重试](rerun/retry-batch-results.json)、[全部数字与清理审计](rerun/evidence-summary.json)。

首批继承的 driver profile 漏开 `engine153-alternate-bank-model`，导致 `v4b-two` 在第二次按下后仍于 PC `0x40c28974` 写 `0x15340004` 被拒。这是本轮配置错误，不能当作已启用 port 后的结果。唯一重试按 port driver 补上该项及 `z8-jpeg-decode=on`，逐检查点 QOM 断言两项为 true；guest 镜像和输入配方相同，On／Off 都走相同菜单路线，只选择不同开关。[最终 driver](rerun/retry-driver/run_live.py)、[预检](rerun/retry-preflight.log)。

[build.py](build.py) 新增 `--encode-speed`，默认 **9**，8 Mbit/s 与 0.5 s IDR 保留；两个编码入口均采用该配置。[speed-9 构建](rerun/build-speed9.json)的镜像 SHA-256 为 `5f93ebc06a7c2ac0b624ea0d320516f100cf619c74ff0f4ff975471d2113ca81`，speed-10 对照与旧镜像逐字节相同。ASan/UBSan 完整尺寸、冻结区／淘汰、取消与两条事务故障矩阵均通过：[主机检查](rerun/host-check.log)。实际 QEMU MOV 的 slice header 也确认 speed 9 的 `disable_deblocking_filter_idc=0`，speed 10 为 1：[码流检查](rerun/qemu-deblock-retry.json)。

下表数字为“原生 JPG 数／Live 组数”；首批名为 `v4b-<用例>`，重试名为 `v4b-retry-<用例>`。所有单拍／Off exit 0，两次 two 均 exit 1。

| 用例 | 首批 | 完整 port 配置重试 | 重试截图 |
|---|---:|---:|---|
| `on-1` | 1／1 | 1／1 | [On-1](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-on-1/step-111-post-encode-30s.png) |
| `on-2` | 1／1 | 1／1 | [On-2](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-on-2/step-111-post-encode-30s.png) |
| `off-1` | 1／0 | 1／0 | [Off-1](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-off-1/step-112-post-complete.png) |
| `off-2` | 1／0 | 1／0 | [Off-2](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-off-2/step-112-post-complete.png) |
| `two` | **1／1，失败** | **1／1，失败** | [最终故障](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-two/final.png) |
| `speed10-control` | 1／1 | 1／1 | [对照审计](rerun/v4b-retry-speed10-control-analysis.json) |

所有原生 JPG 均为 8256×5504、1,169,905 B；四个 Off run 均无 LIVE 媒体，全部三幅 MPO 像素等于原 control，见重试 [Off-1](rerun/v4b-retry-off-1-analysis.json)、[Off-2](rerun/v4b-retry-off-2-analysis.json)。成功配对的 PAIR 像素与各自原生文件相同，JPEG／MOV／MANIFEST identifier 匹配，首帧 IDR、still-image-time sample 正确。

| 完整配置成功单拍 | MOV：Baseline L3.1，1024×682 | 验证 |
|---|---|---|
| `v4b-retry-on-1` | 78 帧且 78 帧不同，3.345755 s，156,470 B，367,943 bit/s；still-image-time 1.829269 s | [媒体目录内 MOV](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-on-1/files/LIVE/100NCZ_8/DSC_0001/CLIP.MOV)、[ffprobe](rerun/v4b-retry-on-1-DSC_0001-ffprobe.json)、[ExifTool](rerun/v4b-retry-on-1-DSC_0001-exiftool.txt) |
| `v4b-retry-on-2` | 78 帧且 78 帧不同，3.334993 s，155,576 B，366,986 bit/s；still-image-time 1.829269 s | [MOV](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-on-2/files/LIVE/100NCZ_8/DSC_0001/CLIP.MOV)、[ffprobe](rerun/v4b-retry-on-2-DSC_0001-ffprobe.json)、[ExifTool](rerun/v4b-retry-on-2-DSC_0001-exiftool.txt) |

同一新 QEMU、同配方／同 port 选项的 guest 编码时间比较如下；均沿用 CNTPCT/CNTFRQ 计时，不含 NV12 转换，包含抢占时间。

| 完整配置 run | 平均／最大 ms/帧 | 相对 speed 10 平均增幅 | 有效 fps | 编码／丢弃 |
|---|---:|---:|---:|---:|
| `v4b-retry-speed10-control` | 23.177／43.138 | 基准 | 23.620 | 1062／1 |
| `v4b-retry-on-1`，speed 9 | 25.642／52.210 | +2.465 ms，**+10.64%** | 23.446 | 1055／9 |
| `v4b-retry-on-2`，speed 9 | 25.531／50.532 | +2.354 ms，**+10.16%** | 23.523 | 1064／5 |

首批同 binary 的 speed-9 平均 25.606–25.622 ms，对照 speed 10 为 22.988 ms，增加 11.39–11.46%；与上一轮约 23 ms 的 speed 10 方向一致，但跨 binary 的旧值不能单独隔离 deblock 成本。状态 `low_fps` 均为 0。重试 On-1 丢帧分解为积压 1＋源检查拒绝 8，On-2 为 0＋5；ring 峰值 492,973／487,901 B，实占 class 0x24 仍为 22,904,832 B。mux 期间分别继续编码 21／18 帧。被动 observer 实测编码函数均在 **CPU index 2** 执行；未采集逐帧指令数。

LV 诊断使用[只读 public-plugin-API observer](rerun/lv_observer.c)与 QMP，记录全部 34 次 >0.5 s 暂停的最后 completion PC／参数／LR、native stop 事件、全核 PC、IRQ 数与实际 hook 指令。observer 只有 host 单调时间用于事件排序，不参与 guest 编码计时；没有 guest 写入、假 IRQ 或 time-control。以下为暂停时的 `irq_seq`，前两项分别为质量菜单／Fenestra 菜单，第三项为收尾停流；two 在 strict fault 前仍持续出帧，所以只有两项。每格链接包含该 run 每次暂停的 PC／完整事件。

| 用例 | 首批暂停 irq_seq | 重试暂停 irq_seq |
|---|---|---|
| On-1 | [388／474／2598](rerun/v4b-on-1-lv.json) | [382／474／2600](rerun/v4b-retry-on-1-lv.json) |
| On-2 | [382／470／2598](rerun/v4b-on-2-lv.json) | [366／458／2594](rerun/v4b-retry-on-2-lv.json) |
| Off-1 | [390／476／2594](rerun/v4b-off-1-lv.json) | [382／474／2594](rerun/v4b-retry-off-1-lv.json) |
| Off-2 | [380／466／2576](rerun/v4b-off-2-lv.json) | [382／474／2588](rerun/v4b-retry-off-2-lv.json) |
| Two | [382／468](rerun/v4b-two-lv.json) | [386／476](rerun/v4b-retry-two-lv.json) |
| Speed 10 | [382／472／2600](rerun/v4b-speed10-control-lv.json) | [372／458／2582](rerun/v4b-retry-speed10-control-lv.json) |

On／Off 都在菜单中由原生 `0x40ea0674 → 0x40e9bb5c → 0x40e90d38 → 0x40c26818 → 0x40c26820` 明确停止 C0；两类菜单暂停在检测时均 requested=0、编码／Live 池申请尚未发生。半按后各 run 均恢复，**没有复现上一轮“半按后仍停、全按前 copied=0”的故障**。例如重试 On-1 的 Fenestra 暂停停在 seq=474，最后完成是 CPU0 的 `live_irq_notify @0x401e219c`，事件 `0x41070030/status=1`、LR `0x401e3238`；停止写入 PC `0x40c26820`、LR `0x40cb8ed0`；采样四核 PC 为 `4016ca60/40189a7c/40189a7c/082013d4`。所有 run 的 hook 分支匹配，IRQ 入口／Live helper 计数相等且等于 irq_seq/2，observer 无丢记录／寄存器错误。因此本轮没有“仅 Live On 停帧”的证据，未触发第 4 项所述 reader/cache/priority/allocator 修复条件；旧轮失败是否属于 QEMU flakiness 仍未因果证明。

**双拍硬阻断：** 完整配置的 `v4b-retry-two` 两次 native key-consumer 相隔 **2.007576 s**，第二 Live 在 mux 时忽略 1 次，符合设计；但卡上只有 `DSC_0001.JPG`。第二张已完成 D980 submit/frame=2、153 bank0、151 jobs/acks=2、三路 NV16 output jobs=2，thumbnail committed generation 仍为 1，JPEG consumed151job 仍为 1。随后 **CPU0 PC `0x4127aa2c` 读 `0x8fff47400`** 触发 strict fault，writer_returns=1；此时 1542 completion/helper 均累计 574 次、irq_seq=1148，LV 并未先停止。见[只读 first-fault-state](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-two/first-fault-state.json)、[原始 stderr](/path/to/user/.local/share/nikon-z8-qemu/live-a-driver-20261104/v4b-retry-two/stderr.log)、[卡与媒体审计](rerun/v4b-retry-two-analysis.json)。断点与 port 报告的第二张 A2 thumbnail／原生通知缺口一致；本轮证明约 2 s 间隔也未通过，不能只归于 0.333 s 双按。按要求未重建 QEMU 或放宽其 strict rejects，整体双拍验收仍失败。

全部批次结束后才写此节；大 raw/bin 已清理，保留日志／JSON／PNG／媒体。`sleep=off`、ext4、sparse 复制、资源检查均执行，启动可用内存最低 17.4 GiB；实际相邻启动间隔 **101.25–102.03 s**（100 s 门槛加轮询开销，略长于 90–100 s 目标），审计时 C: 剩余约 215.8 GiB。未刷机、制作更新容器、修改共享 QEMU、commit/push。旧报告列出的硬件、DMA lease、曝光锚点和导入兼容性未证明项仍保留。
