[推论] **照片模式独立使用 Wave5 有明确的底层接口基础，但尚不能视为可安全执行的补丁。** 本轮接通了 stock C3.11 的硬件上电、微码、RTOS、IRQ、电影内存分配及退出链。最值得优先追踪的原生替代入口是“间隔拍摄 → 延时视频”，它支持同时保存照片和视频；尚未证明存在直接接受任意 YUV 队列的高层 API。

[实证] 本轮仅静态反汇编、表解析和官方文档研究，未运行 QEMU、未改目录外文件、未提交。已阅读题目指定的五份 sibling REPORT、`z8-video-data.md` 和 `I5-log.md`。下文“实证”区分 stock 指令/数据、官方说明及前次模型记录，**不代表本轮实机验证**。REA 会话调用因当前审批策略拒绝启动，遂使用指定 Python `-I` 与 Capstone；证据文件保留在本目录。

[实证] MAIN：81,807,709 字节，装载基址 `0x40160000`，SHA-256 `2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853`。下文地址均为该版本 VA，MMIO 地址另行注明；MiB 为 2²⁰ 字节。[固件身份](FIRMWARE-IDENTITY.txt)

**1．电影进入、运行及退出链**

| 阶段 | stock 地址及行为 |
|---|---|
| [实证] 常驻服务准备 | `0x402f2a34 → 0x40c32d44` 建立 HEVC 信号量、事件和两个任务；同时 `0x402f73c0` 预留微码 staging。`0x402f2a58 → 0x40c32f8c` 安装 IRQ。它们与每次开关硬件不是同一步。 |
| [实证] 电影内存申请 | `0x407250b8` 按 `0x440d29c8/0x440d29e8` 的 resource/class 对调用 `0x4104d2dc`；descriptor、实际地址、大小保存在 `0xa046135a8 + resource*0x20`。`0x40725870 → 0x40726820 → 0x40d056a0 → 0x40d069d0` 交给电影服务。 |
| [实证] 电影服务/适配器 | `MOVRS_F` 配置索引 `0x22`，初始化 `0x40d06c50`，处理函数 `0x40d06d3c`，对象 `0xa031b5f38`。其 buffer 表为 `0xa031b5ec8`；`0x402f6be0 → 0x402f5624` 绑定 codec 缓冲区，`0x402f6be8 → 0x402f584c` 取得 core arena。回调 `0x40d07f1c` 经 `0x402f6bd4` 注册。 |
| [实证] 硬件入口 | 电影调用点 `0x40d07ba4 → 0x402f2d58(W0=client, W1=速率相关值, X2=arena, W3=size)`。client 范围 0–4；另有 `0x40cf9854` 调用，不能把此入口等同 REC 按键。 |
| [实证] 驱动请求 | `0x402f3038 → 0x40c32fec → 0x40c37aa4`：获取 slot/锁，复制 0x28 字节请求到 `0xa02d32720`，对事件 `0xa02d30070` 发 bit 1、等 bit 2；等待参数 `0x3e8`，时间单位未定。请求含微码半字数、staging 指针、arena 大小/地址和总线配置。 |
| [实证] 工作任务 | 索引 `0xbb/0xbc` 对应 `HEVC_0/HEVC_1`，入口 `0x40c3a454/0x40c3dd5c`，对象 `0xa02d300a0/0xa02d300d8`。名称表由 `0x41186a28 → 0x40c011e4` 设为 `0xa029c19e0`，每项 0x1c。 |
| [实证] core 初始化 | worker 在 `0x40c3b0d0` 调 `0x40c3fae8` 保存 arena/高地址位；在 `0x40c3b0f4` 进入 `0x40c34f78 → 0x40c42740 → 0x40c42658 → 0x40c40858 → 0x40c54374`。VDI 状态 `0xa02d3d928`，MMIO 基址字段 `+0x58 = 0x16010000`。 |

[实证] 上表的直接调用、分配与任务证据：[生命周期](lifecycle.txt)、[服务初始化](init-service.txt)、[电影分配](movie-allocation.txt)、[分配表](allocation-tables.txt)、[内存交接](memory-link.txt)、[任务表初始化](task-table-init.txt)、[任务表返回值](task-and-boot-tail.txt)。这些是关键调用边及异步交接，并非把所有函数误写成同一同步调用栈。

[实证] `0x402f2d58` 获取设备 ID `0x156/0x1de/0x1e4`，句柄位于 `0xa02e98488/490/498`，经 `0x40c2d0f8/0x40c2e7fc/0x40c2fb7c` 加锁；按以下次序操作。表中的“隔离/电源/时钟”是根据行为作出的功能解释 [推论]，每一 IP 的硅片域名、共享范围及实际 MHz 仍 [待验]。

| 次序 | 调用位置/底层函数 | 可直接核对的寄存器行为 [实证] |
|---|---|---|
| 1 | `0x402f2df8..2e0c → 0x40c2ea74`，ID `0x26/27` | 对 `0x0d150304` RMW 清 `0x200/0x400`；反向 `0x40c2e9f4` 置位。 |
| 2 | `0x402f2e10..2e30 → 0x40c2fd48 → 0x40c2fb84` | 依次请求 `0x400000/0x800000/0x1000000`。控制器基址 `0x0d158000`；读 `+0x18`，ON/OFF 差量写 `+0x20/+0x40`，触发写 `0xaa00 | on | (off<<1)`；等待事件 `0xa02c98644`。反向入口 `0x40c2fd68`。 |
| 3 | `0x402f2e34..2e9c → 0x40c2d1e4` | 关闭 ID `0x79..81`；表 `0x4164c278` 给出 `0x0d1520c8` 的 shift `{14,16,18,20,22,24,26,28,30}`，逐项写 `3<<shift`。 |
| 4 | `0x402f2ec4/2ed4/2ee4 → 0x40c2d100` | 表 `0x4164c1e8`：`0x0d152008` 的 shift `{0,16,20}`；ID 4 写 8 或 9（原 W1 ≥240000 时为 8），ID 5 写 8，ID 6 写 0xe。不是已知频率值。 |
| 5 | `0x402f2ef0..2f50 → 0x40c2d8cc` | **九个** reset ID `0x43..4b`；表 `0x4164c494` 映射到 `0x0d154248`，逐项写 `1<<16` 至 `1<<24`。 |
| 6 | `0x402f2f54..2fbc → 0x40c2d19c` | 开启上述九个时钟，逐项写 `2<<shift`。这是原生控制写序列，不应当作普通寄存器全值恢复。 |

[实证] 题目旧模型日志的 `0x10000..0x400000` 仅覆盖七个 reset；stock 还操作 `0x800000/0x1000000`。[待验] 不能据此认定九个全部专属 Wave5，或保证整组 reset 不影响 still/ISP。[底层实现](bringup-details.txt)、[静态表与计算](STATIC-SUMMARY.txt)

[实证] 微码不是从存储卡临时加载：`0x402f7450` 将 **MAIN 内 `0x42a01068`（文件偏移 `0x28a1068`）的 `0xa3e80` 字节**复制到 `[0xa02ede828]`，返回 `0x51f40` 个 16 位字。staging 由 `0x402f73c0 → 0x41186688(0x100000)` 在常驻递增 arena 中取得；其初始游标为 `0xa170fe400`，因此不可把此值当作 staging 固定地址。微码 SHA-256：`ad1eaa5ad142ffa537459ada0a02661346f74e7f686c75c1618c5ae15e922a53`。[复制路径](bringup-details.txt)、[分配来源](alloc-sources.txt)

[实证] `0x40c3e994/0x40c3eda8` 从调用者 arena 建立 **2 MiB common buffer**；`0x40c54374` 经 `0x40c3f31c` 复制微码，code 占首 1 MiB，TEMP 位于下一 1 MiB。正常电影传入的 arena 是 16 MiB；VDI 的“size=0 默认 128 MiB”不代表自动得到这块内存。boot 清 `MMIO+0x100..1fc`，设 remap `+0x60=0x80000900`、`+0x64=0`、`+0x68=code低32位`，设 code/TEMP `+0x110/114/11c/120`，配置 FIO 高位映射和 SRAM `+0x124/128`，设 IRQ mask `+0x48=0x8340`，写 busy `+0x70=1`、command `+0x100=1`、core-start `+0x6c=1`；等待 busy 清除并检查 `+0x108/10c`。初始化后还有 `0x4000` query/version 流程。[核心代码](wave5-core.txt)、[boot 返回检查](task-and-boot-tail.txt)

[实证] IRQ 是 **INTID 120**：`0x40c32f8c` 经 `0x40c00710` 注册 `0x40c35774`。ISR 读取 `+0x44/4c` 和 instance 位图 `+0x1e4/1e8/1fc`，经 `0x40c34d3c → 0x40c39e58` 通知服务；`0x40c35a04..35a20` 写 `+0x34` reason、`+0x3c=1` 清中断。这是 Z8 stock 直接证据，不只来自其他机型或模型。[驱动](wave5-driver.txt)

[实证] buffer class 的链已接通：resource `{0,4,5,6,7,8,9}` → class `{3f,3e,46,47,48,3b,3c}` → 服务 slot `{0,1,2,3,4,5,6}`；codec adapter 消费 slot `0/2/3/4` 作为 type `0/1/2/3`。下表容量由 stock `0x402f55a4/5624` 与 sibling RAM 表交叉核对，HEVC 列为非 proxy 分支，**不是指定分辨率的最低需求**。

| class / type | HEVC / AVC 容量 | adapter 中的用途及地址 [实证，语义另注] |
|---|---|---|
| `0x3f / 0` | 320 / 128 MiB | `0x402f5648` 设置双码流区，各 160 / 64 MiB；HEVC proxy 分支各 90 MiB。状态在 `0xa02e98e00+0x50..64`。 |
| `0x46 / 1` | 489 / 42 MiB | `0x402f567c` 对齐 4 KiB：首 16 MiB 为 core arena；随后 467 / 20 MiB 的 frame backing [推论：参考重建区]；另有 512 KiB、5 MiB、`0x7f000` 子区。不能把全部 489 MiB 称作微码或 TEMP。 |
| `0x47 / 2` | 8 / 8 MiB | `0x402f56d4` 绑定对齐后的 `0x7ff000` 字节辅助区；具体消费者仍 [待验]。 |
| `0x48 / 3` | 130 / 47 MiB | `0x402f56fc`：HEVC 两块 61 MiB，加 5 MiB 等尾部；AVC 三块 13 MiB，加 5 MiB 等尾部。并非 90 帧的历史 ring；各块完整语义 [待验]。 |

[实证] 具体 VPU 内存申请 `0x40c3f760` 还依赖绑定表，不能替换为几个裸地址：tag `8/0x12` 使用 VDI arena allocator，其他 tag 经 `0x40c34e3c → 0x40c3a100`。encoder 表 `0xa02d308f0 + instance*0xf0` 中，tag `0xa` 取 `+0x28` 指针/`+0x20` 大小（WORK）；tag `0xc/0xd` 取 `+0xd0/+0xe0`（FBC 表），tag `0x10` 取 `+0x38`（MV col），tag `9` 取 `+0x18`。CREATE 的一个产品分支请求 WORK `0x20000`；不能把此数推广成所有产品/模式。SET_FB `0x40c58f54` 按尺寸、对齐和数量计算辅助需求并注册参考帧，**source YUV 与参考重建帧必须分别拥有**。[缓冲切分](buffers.txt)、[tag 分配](allocator-dispatch.txt)、[参考帧注册](register-fb.txt)

[实证] 正常退出：电影 `0x40d0712c → 0x402f3084`，按 client 找 slot，`0x40c3325c/0x40c37bac` 关闭实例；最后引用退出经 `0x40c37b38` 发 event 4、等 8，worker `0x40c3b5dc → 0x40c34fb8 → 0x40c42784 → 0x40c3ec04` 释放 VDI。board 层随后用 `0x40c2d884` 向 **`0x0d154348`** 重新断言九个 reset，关时钟、关三组电源请求、恢复隔离并解锁。电影 backing 由 `0x407257f0 → 0x4104d600` 释放；常驻 staging 与 RTOS 服务不应每个 clip 重建/销毁。[推论] 原生错误分支存在记录失败后继续清理的情况，不能原样移植成照片并行环境中的无条件整域复位。

**2．哪些依赖电影模式**

| 部分 | 独立编码的判断 |
|---|---|
| [实证] Wave5 输入 | picture 入口 `0x40c59890(X0=instance,X1=picture参数)` 从描述符读 plane 地址，写 MMIO `+0x148/14c/150`，stride 写 `+0x154`、格式写 `+0x158`；输出地址/容量写 `+0x118/11c`。它不直接向 sensor 请求帧。[推论] 已拥有且可 DMA 访问的 YUV 可以作为输入。 |
| [推论] sensor、151/1518、视频 ISP | 它们是取得/变换电影画面的上游条件，不是“编码已经存在的帧”的必需条件。使用已捕获 YUV 不需要为编码而切 sensor mode。原生 adapter 的 type 4 绑定 `0x200000000` 输入别名；这不是可以无条件沿用的 PHOTO 内存地址。 |
| [实证][推论] 4K PHOTO 输入 | 前次 [live-probe](../live-probe/REPORT.md) 模型记录为 B0 640×480 NV12、B1 1024×680 NV12、B2 1920×1080 NV16；没有 PHOTO 4K producer 证据。1080p NV16 需验证能否原样输入，否则先转 420。放大到 4K 不增加原始细节；仍需单独解决真正 4K 的 sensor/ISP 支路。 |
| [推论] 音频、MOV mux、REC UI | core 命令与逐帧 YUV 编码不需要音轨或 REC UI；独立 elementary stream 可不启用它们。但电影 recorder 状态机有队列、callback、EOS、文件收尾依赖，不能只隐藏 REC 标志就复用整个状态机。旧 I5 音频空队列阻塞是模型侧诊断，不是硅片性能证据。 |
| [实证][待验] 内存与共享服务 | 现有驱动依赖常驻任务、IRQ、锁、slot、VDI 和绑定表；可以复用其协议，但本轮没有证明 PHOTO 已完成全部初始化。reset/power 域与 still、HEIF、LV 的共享边界、DMA 地址翻译、缓存一致性仍须确认。 |

[实证] 上述 source 寄存器与入口见 [逐帧代码](final-traces.txt)、[入口与 stride](class-binding.txt)。题目中的 `0x40c58238/0x40c5a448` 是 **BL 命令调用点**，不是可直接当函数入口的地址；`0x40c59d18` 只是准备 `W1=0x100`，实际 BL 在 `0x40c59d20`。

[待验] ±1.5 秒的有效新帧和光学曝光锚点仍须独立验证；原有完成事件不等于内容更新。当前没有可直接保活全部 LV buffer 的通用 retain 证据，必须在可靠完成/所有权边界复制到自有 ring，不能持有裸指针等拍后编码。[取帧限制](../live-probe/REPORT.md)、[曝光及停启限制](../live-blackout/REPORT.md)

**3．最小独立流程、RAM、时间和失败处理**

[推论] 对 N 张已经捕获且自有的 YUV，最小协议如下；这是实现设计，不是经过验证的调用脚本。全分辨率 still 保持原生 RAW/JPEG/HEIF 处理，优先等其 DMA/写盘关键阶段结束，再开始附加编码。

1. [推论] 确认没有电影/回放 codec client 冲突，使用原生所有权协议占用缓冲区并核对实际返回大小；记录此次取得的 slot、锁及各阶段完成状态。保留 PHOTO memory plan，不能为了申请电影 class 强制切 selector。准备输入 plane、stride、格式、色彩参数及 DMA/cache 同步。
2. [推论] 复用已经存在的 HEVC 任务和 IRQ；在确认域独占后，按 `0x402f2d58` 的流程启动硬件、加载内嵌微码、检查 init/query 结果。必须同时建立 VDI arena 和 encoder tag backing，不能只执行 MMIO 序列。
3. [实证][推论] 经 **CREATE 入口 `0x40c57ff4`** 发 `8`；设置宽高、帧率、码率、8-bit 420、GOP 等并发 SET_PARAM `0x200`（调用点 `0x40c584b8/0x40c58cfc`）。[推论] 首版采用单实例、无 B 帧、首帧 IDR 的短 GOP，减少重排和排空复杂度；所需参考/源帧数量按 sequence 结果分配，不能固定猜一个数。
4. [实证][推论] `0x40c58f54` 注册参考帧并发 SET_FB `0x80`（BL `0x40c5982c`）；header 编码路径 BL `0x40c5a334` 发 `0x100`。[推论] 保存 AVC SPS/PPS 或 HEVC VPS/SPS/PPS，再逐帧调用 `0x40c59890` 发 `0x100`；通过 IRQ、结果 query 和输出指针取得实际码流，及时消费，只有确认硬件不再读取的 source 才可复用。
5. [推论] N 帧后发原生 EOS、排空延迟输出，检查输出帧数/错误/剩余码流；调用 **FINISH 入口 `0x40c5a43c`** 发 `0x20`，等待完成，再关闭 slot、core，按所有权反向释放。所有命令经 `0x40c541e0` 串行化，不能由第二线程绕过已有服务直接打寄存器。
6. [推论] elementary stream 另行封装 MOV，使用真实 capture 时间定位快门；添加 Apple 配对 ID 和 still-image-time metadata。选用原生全分辨率 JPEG/HEIF 作为照片资产，RAW 可照常保留；不能以视频抽帧替代原生 still。[待验] 既有 Photos 成功样例使用视频抽帧封面，不能证明 native still 或 RAW 本身能按同样方式配成 Live Photo。

[实证] 普通 PHOTO 可通过 `0x4104d2dc(caller, logical class 0x60, size)` 取得 descriptor（通常映射 `0x24`），按 `D+8 → R` 读取实际地址/大小，配对 `0x4104d600(D)`。但这会占用照片池，申请成功也不证明对连拍容量和换模式无影响。旧 movie 160 MiB 区间 `[0x857400000,0x861400000)` 与 PHOTO class `0x24` 重叠，**不能裸借**。[前次 RAM 证据](../live-ram/REPORT.md)

[实证] `0x40c32fec` 对 arena 要求 `(base & 0xfffffff800000fff) == 0x800000000`，即特定高地址范围且 4 KiB 对齐。[推论] 实现须沿用高地址/FIO 映射与 cache 同步，不能把任意 CPU 指针截成低 32 位交给 VPU；原生电源握手内部也不能假定受外层 `0x3e8` 等待限制。

[实证]（算术） 30 fps、±1.5 秒共 90 帧，紧凑排列且无 padding 的历史图像负载如下；实际 stride、allocator 取整、所有权余量还需另加。[复算脚本](summarize_static.py)

| 格式 | 前 45 帧 MiB | 全 90 帧 MiB |
|---|---:|---:|
| 3840×2160 8-bit NV12 | 533.936 | 1067.871 |
| 1920×1080 8-bit NV12 | 133.484 | 266.968 |
| 1920×1080 NV16 | 177.979 | 355.957 |
| 1024×680 NV12 | 44.824 | 89.648 |
| 640×480 NV12 | 19.775 | 39.551 |

[推论] 若收满后才启动编码，必须支付完整 N 帧；只存前半段要求在后半段采集中持续消费，增加与 still 并发的验证范围。第一版宜逐帧转换，避免同时保留整段 NV16 和 NV12；10-bit 输入和参考布局也不能沿用上表 1.5 B/pixel。

[实证][推论] 不裁剪原生配置时，仅 class `3f/46/47/48` 合计 HEVC **947 MiB**、AVC **225 MiB**，还不含其他电影类和自有历史 ring；例如 90 帧 1080p NV12 + AVC 四类已约 **492 MiB**，4K NV12 + HEVC 四类约 **2015 MiB**。这说明直接搬整套电影内存很昂贵，不是它们在 PHOTO 必然可分配。

[推论] 收缩版预算应按 `M = 历史帧 + core arena + WORK + 参考重建帧 + FBC/MV等辅助 + 码流窗口 + 转换scratch + 元数据` 逐项计算。保留 stock 的 16 MiB arena 和 512 KiB WORK 预留可作起点；额外 staging 如尚未预留再加 1 MiB。参考帧/辅助区依产品、尺寸和 sequence 返回值确定，码流窗口可流式写出而小于整段。[待验] 尚无经验证的“4K 独立编码器只需 X MiB”下限，不能把 947 MiB 直接改成经验数。

[实证]（算术） 3 秒 10–20 **MB** 对应约 **26.7–53.3 Mbit/s**（若指 MiB 则为 28.0–55.9）。[推论] 应给 MOV 开销留余量，码率目标不等于文件硬上限，超限必须停止附加 clip 而不越界覆盖。

[推论] 从开始编码算，`T ≈ T_init + N/F_effective + T_conversion/cache + T_drain + T_IO`（有重叠时不能机械相加）；若等后半段采满，还要加快门后的 1.5 秒及 still 等待。[实证]（条件算术） 90 帧在假设有效 30/60/120 fps 下纯编码为 3/1.5/0.75 秒。[待验] Z8 在此并发负载下的有效 fps、微码启动时间和功耗均未知；QEMU host x265 不执行 Nikon VPU 微码，不能用来估计硬件速度。

[推论] 失败处理应以保住原生照片为优先：申请/格式/能力检查失败时尚不触碰硬件；每个等待须有经校准的期限，并从独立 worker 运行，不能卡住照片线程。限制队列和输出容量，记录 INIT/实例/帧/EOS/FINISH 状态；取消、连拍、模式切换及关机也走同一清理状态机。只清理本 job 已取得的引用，不销毁全局 IRQ/任务，不修改仍被其他 client 使用的时钟域。

[待验] **超时后“立即 reset、free 所有 RAM”不是已证明安全的恢复。** DMA 可能仍在访问旧 buffer，九个 reset 的共享范围也未知；须先确认 DMA/core idle 或证明专属 reset 可终止访问，再归还内存。无法证明时停止后续附加任务并保留仍可能被 DMA 使用的内存，交由已验证的原生恢复/关机流程处理。现阶段尚不能承诺任何故障都不影响相机。最低实机验收应覆盖原生 still 完整性、逐帧完成/缓存一致性、重复启停、超时、取消和模式切换；这不是本轮已执行项目。

**4．可复用的原生“非普通录像”路径**

| 候选 | 结论与证据 |
|---|---|
| [实证] 间隔拍摄的 Time-lapse video 选项 | 官方明确说明同时保存照片和延时视频，并可配置视频格式/尺寸。[推论] 这是最接近“原生 still + 硬件视频”的复用候选，应优先追踪其 frame 提交、缩放、codec client 与结束链，而不是先重写 raw bring-up。[官方说明](https://onlinemanual.nikonimglib.com/z8/en/psm_interval_timer_shooting_151.html) |
| [实证] 照片菜单的 Time-lapse video | 官方支持按间隔自动拍照生成视频。[待验] 不代表提供现存 YUV 列表 API，也不证明正常 LV、普通快门和编码可并行；其采样时间与用户要求的实时 ±1.5 秒不同。[官方说明](https://onlinemanual.nikonimglib.com/z8/en/psm_time-lapse_video_152.html) |
| [实证][待验] 回放 Trim video | 官方确有裁剪并保存副本，但文档不说明是码流复制、重封装还是重编码；不能宣称已发现 memory encoder，也不能反向断言必然只 remux。[官方编辑说明](https://onlinemanual.nikonimglib.com/z8/en/editing_features_on_videos_73.html) |
| [实证][推论] Save current/consecutive frames | 官方功能是从视频保存静态帧，支持范围列为 H.265 8-bit/H.264 8-bit；方向是 decode→still，不能证明存在 YUV→video 服务。[官方编辑说明](https://onlinemanual.nikonimglib.com/z8/en/editing_features_on_videos_73.html) |
| [待验] HDMI | 本轮没有找到 HDMI 输出支路调用 Wave5 编码的闭合证据。外部录机输出本身不足以证明机内 memory encode；不能当作已找到的复用接口。 |

[实证] stock 有 `m_arRelatedTimeLapseMovieImageInfoDataIdList` 字符串（`0xa02a61626`），且硬件 bring-up 有电影调用之外的 client 调用；[待验] 本轮未闭合该字符串到具体延时视频创建/提交函数的引用链，也未确认另一 client 就是延时视频。官方在线手册可证明功能存在，不能替代 C3.11 内部调用证据。[候选字符串](relevant-strings.txt)、[调用交叉引用](lifecycle-refs.txt)

[推论] 实现优先级：先追通间隔拍摄的双保存路径及 PHOTO 帧保活；若无法复用，再在现有 HEVC 服务之下实现独立内存输入 client。首轮用已有 PHOTO 帧源和 H.264/HEVC elementary stream 证明完整启停；真正 4K 输入、快门附近缺帧、native still 的 Apple 配对及可恢复性是独立验收项，不能以“VPU 已发命令”代替。
