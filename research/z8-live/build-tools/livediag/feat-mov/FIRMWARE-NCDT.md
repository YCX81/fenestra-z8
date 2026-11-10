# Z8 Live MOV 的 NCDT、原生回放与 Apple 配对

**在当前 C 3.11 MAIN 的模式 0 解析器中，06 已经可以打开，08 还可以读出并解析 Nikon metadata。** 08 的 H.264、PCM 和时间表来自真实 Live 成片，四张缩略图来自较早的 QEMU 彩条录像。实机播放、声音、DCF 登记与返回原照片仍待验证。

## 当前 MAIN 读这组 MOV 的结果

在 `0x40344b20` 所用的 MOV 模式调用 `0x40ce8a08`，再按 worker 的顺序取 atom ID `0x44` 的长度和内容。下表为运行 ARM64 指令得到的数值。低层解析、盒长度查询和内容复制用 0 表示成功。NCDT 外层用 1 表示成功，身份状态 0 表示厂商、机型和版本前缀全部匹配。

| 文件 | 低层解析 | udta 查询／复制 | NCDT 外层／身份 | 从这个对照可知 |
|---|---:|---|---|---|
| 00 原件、02 只去 mebx、03 只换 stco、04 只修尺寸 | 6 | 未进入 | 未进入 | 文件偏移 20 的四字节不匹配 `niko`。 |
| 01 只加 niko | 6 | 未进入 | 未进入 | 仍有 `meta` handler。 |
| 05 加 niko 并去 mebx | 6 | 未进入 | 未进入 | 此时只需把音频 offset 表从 co64 变为 stco 即得到 06。 |
| 06 再统一音频 stco | 0 | 3／未进入 | 未进入 | 容器 1024×682、SPS 1024×688 的差异没有阻止这层开文件。 |
| 07 容器跟随 SPS 到 1024×688 | 0 | 3／未进入 | 未进入 | 单独修尺寸没有生成 Nikon metadata。 |
| 08 在 07 加入 donor NCDT | 0 | 0／0，324,169 B | 1／0 | 低层确实从 MOV 读回 `udta`，其中的 NCTG 可通过身份检查。 |
| 09 在 06 仅为 SPS 设置底部裁 6 行，显示 1024×682 | 0 | 3／未进入 | 未进入 | 当前模式可打开该 AVC 描述，仍无 udta。 |
| 归档 QEMU HEVC／24-bit PCM，有旧 Apple 定时轨 | 6 | 未进入 | 未进入 | 在同一判定模式下，有合理的 NCDT 也不能跳过较早的轨道检查。 |
| 从上述归档去掉追加的 Apple `moov/meta`、定时轨和 marker mdat，合并 free 并保持原 AV 数据偏移 | 0 | 0／0，324,169 B | 1／0 | 原厂 writer 布局的离线对照。此文件仅在测试期间构造。 |

原始分支位置为 `0x40ce8fb8` 的固定品牌、`0x40cea490` 的 handler 分类，以及 `0x40cea9cc` 的音频 chunk 表选择。针对 00–05，表中 6 是函数原始错误码，原因结合文件的单变量差异和反汇编得出。真实回放 UI 何时分派模式 0、如何解释 6、何时调用硬件解码器仍未闭合。[历史分支记录](history/astra-b00huynz8.md) · [测试矩阵](tests/MATRIX.md)

## 从原厂封装器产物恢复 NCDT

工作区迁移档里有 11 个带 NCDT 的 MOV，来自 C 3.11 固件在 QEMU 中录像，再由当时的 Live 扩展增加 Apple 配对。选用 `pair-u02a/DSC_0002.MOV`，其主画面为 3840×2160 HEVC、音频为 48 kHz 24-bit PCM。文件 SHA-256 为 `343ad5b65159ae9a8b23229f70e8bd39db73bc4a9b00119662180906d413f30c`。从文件偏移 1,939 起是 324,169 B 的 `udta`，第一项 NCDT 自 1,947 起共 324,161 B，含盒头 SHA-256 为 `a028c028bf94460e1fe12b6a14dd890dde7aa4dda742cdd36446a0325192bf87`。[源 MOV](../../../_mac-migration/live-session/live-photo-20261030/pair-u02a/DSC_0002.MOV) · [归档录制说明](../../../_mac-migration/live-session/live-photo-20261030/RESULT.md)

| 子盒 | payload 字节数 | 字节检查与消费位置 |
|---|---:|---|
| NCHD | 10 | `4e 69 6b 6f 6e 00 02 00 00 00`，版本 2。校验从 `0x406bf590` 开始。 |
| NCTG | 5,139 | 82 条连续记录，每条以大端 tag:u32、type:u16、count:u16 开头，紧跟 count 个该类型的值。最后一条恰好到盒末尾。 |
| NCTH | 5,538 | 160×120 baseline JPEG。 |
| NCVW | 208,321 | 3840×2160 baseline JPEG。 |
| NCM1 | 18,131 | 640×360 baseline JPEG。 |
| NCM2 | 86,958 | 1920×1080 baseline JPEG。 |
| NCDB | **0** | 有一个总长 8 的盒，没有 OP01 或 OP02。删除整盒后外层失败。独立 NCDB 数据复制接口另有非空检查。 |

四个图像 payload 都是从首字节 SOI 到最后 EOI 的完整 JPEG，交给 `djpeg` 均完整解码成功。11 个样本的四个 JPEG、NCHD 和空 NCDB 分别逐字节相同。82 条 NCTG 记录中只有 `0x13`、`0x31..34` 和 `0x02000091` 的值在这批样本之间发生变化。彩条、原厂模拟设置和旧录像的身份信息只适于对照。

### NCTG 中能和当前读函数对上的记录

在 `bg34.main` 找到 97 项初始化表的 ROM 副本 `0x44dea658`，每项 64 B。97 对 group/tag 与 `0x440c76d0` 的独立常量列表逐项相符。驱动将表复制到 `0xa0297f658` 后执行原始 `0x406ce364`，填充解析字段指针；真正执行查表、记录回调和身份比较的函数是 `0x406ce344`。

| tag | 类型／count | donor 内容及含义 |
|---|---|---|
| 1 | ASCII／18 | `NIKON CORPORATION\0`，写入解析结构 +0，比较 `NIKON` 前缀。 |
| 2 | ASCII／16 | `NIKON Z 8` 后补零，写 +0x12，比较机型含结尾零。 |
| 3 | ASCII／32 | `NIKON Z 8 Ver.03.11` 后补零，写 +0x22，比较机型前缀。 |
| 0x22、0x23 | 各 u16／1 | 3840 与 2160，和 donor 主视频相符，字段去向为 +0xa4／+0xa6。 |
| 0x101 | u16／4 | 160、120、3840、2160。后两个数进 +0xc4／+0xc6，控制 NCVW 查询。 |
| 0x102 | u16／8 | 640、360、1920、1080、0、0、0、0。头两对给 NCM1 和 NCM2。 |

**能通过哪一层。** 保留真实的四个 JPEG、NCHD 和空 NCDB，只保留 tag 1、2、3、0x101、0x102，NCTG 从 5,139 B 降至 130 B。当前 parser 返回外层 1、身份 0，四项图像查询均得到原尺寸。只保留两条图像记录则身份 2，四项全失败。保留厂商及两条图像记录时身份 1，四项仍通过。只保留三条身份记录时身份 0，只有不依赖图像尺寸记录的 NCTH 成功。此前的 66 B 全空 shell 实跑为外层 1、身份 2、四项全失败。五条记录是对这些函数验证过的充分子集；播放器对其余记录的需求和可播文件的最小大小仍未知。[读函数反汇编](evidence/nctg-disassembly.txt) · [图像查询反汇编](evidence/ncdt-disassembly.txt)

**1280×852 的两处尺寸可以独立检验。** 在 donor NCTG 仅将 `0x22/0x23` 改为 1280、852，四张图的查询全通过。若只把 `0x101` 的后两值改为 1280、852，外层和身份仍通过，唯独 NCVW 查询失败。两个实验均保持 JPEG 字节及 HEVC 码流不动。这说明 `0x406c0dd0` 检查的是相应预览图的声明尺寸。生产 writer 可按实际 AVC/SPS 写视频几何，另生成 16:9 的 JPEG 预览，例如 1280×720 的 NCVW，并在 `0x101` 描述这张图。1280×852 正片是否可被实机解码，需用实际编码成该尺寸的 H.264 成片测试。

## 固件应该改在哪里

1. `src-fix31/codec/live_mov.c` 的 `live_mov_begin` 和 `live_mov_finish_with` 共同维护 mdat 起点。品牌扩展、实际 chunk 地址、尺寸与方向矩阵、metadata track 分流都要在 writer 里实现。`audio/live_audio_mov.c` 写 checked 32-bit stco。NCDT 模块应使用本次照片或关键帧生成图像，元数据填本次录像参数。当前 `live_codec.h` 中 `atoms[16384]` 容不下 324,169 B 的 udta，先写后查长度会越界。可预计算盒长度，先写 moov 头和小表，再分段写 udta、JPEG；各写操作检查容量和结果，最终回填长度。固定拷贝 donor 会让每张 Live 显示同一幅旧彩条。
2. `codec/live_transaction.c` 中 MOV 关闭后才开始 LVP 和 MAN。引入可识别的新 manifest 版本，把唯一的相机 MOV 位置／DCF 身份、所选快门帧时间、UUID、方向一同提交；MAN 仍最后写入。现有 `pbfix/live-playback-ui-icon/sync.c:37` 遇到未知键就报错，新 reader 必须先能识别两种版本；旧文件组继续按原路径删除／保护。
3. 卡上 MOV 能从电影项目播放后，再接 still 页的 Live 按键。原生入口 `0x40cf3ffc` 复制调用方的 0x48 字节结构，`0x40719b44` 一带按文件对象 ID 工作。先验证原厂 DCF 分配、登记电影、启动消息、停止后恢复原 still。给同目录 JPG/MOV 套用相同四位编号不足以确认它们能作为两个独立对象被选中。交还编码器所有权与退出回调还需单独验证。
4. 以上路径走通后，删除新事务里的 LVP create/write/close/回滚，相应去掉 `capture/live_stream.c` 的 preview ring、ticket 的 preview 回调、`pbfix/live-playback-ui-icon/runtime.c` 的逐帧 LVP 读与大 RGBA 绘制。保留照片徽章、旧 MAN/LVP 兼容、删除和保护同步；资源所有权以仍在使用的编码／播放任务为准。当前每段 LVP 可省 47,139,872 B（按 32 B header、45 帧 1024×682 I420 计算）。[完整改动清单](firmware-plan.md) · [播放对象反汇编记录](history/astra-bavfudld4.md)

### iPhone 配对如何保留

真实 `DSC_9776.JPG` 的 Apple MakerNote 0x0011 与样本 MOV 的 `moov/meta` 使用同一 UUID `9AA7B7CE-B1CA-6F97-2DAE-206CE91E41FF`。MOV 的 `meta/mebx` 单样本为 `0000000900000001ff`；真正的定格时间来自该轨的 edit list。本片 movie timescale 为 1,000,000，`elst` 先空过 **1,735,084 μs**，再显示 **144,655 μs**。旧 writer 选取 PTS 不晚于按键的最后一帧，因此保存该帧的精确相对 PTS 和 duration 比用总时长除二更可靠。[写入代码](src-fix31/codec/live_mov.c) · [配对照片](../../../live-export/DSC_9776/DSC_9776.JPG)

相机保存配对 JPEG、camera MOV 和小 MAN。导出到手机或电脑时，按 MAN 在副本中恢复 movie 级 UUID 和 timed mebx（`cdsc` 指向视频轨、正确的 `elst`、`stts`、真实 marker sample 与偏移表）。视频和音频可以原样复制。验收时先复核导出文件中两侧 UUID、marker PTS、方向和每轨样本，再让 iPhone 实际导入并播放。归档的 `pair-u02a` 有用户在 Mac「照片」识别 Live 的记录，本次从 camera MOV 再导出的文件尚无 iPhone 导入结果。[旧样本的用户记录](../../../_mac-migration/live-session/live-photo-20261030/RESULT.md)

## 复现与下一份实拍材料

在 Nikon 工作区根目录运行以下命令。依赖已在本机的 clang、QEMU 10.1.1 及带 pyelftools 的 Python。脚本校验 MAIN 与 donor 的 SHA，打印每组实际返回值。驱动使用只读内存文件代理代替 seek/read；它为解析器提供 5 MiB 工作区，在 NCDT 文件服务交付后的 `0x406bf244` 进入内容分析。为跳过未启动的文件服务清理，它仅在模拟器 RAM 将 `0x406bf1f8` 指向原有返回尾部。没有启动 Z8 操作系统、写回 MAIN 或访问设备。[Python 驱动](evidence/check_ncdt.py) · [ARM64 驱动](evidence/ncdt-replay.S)

```sh
build-tools/livediag/feat-rec/.recorder-venv/bin/python -B build-tools/livediag/feat-mov/evidence/check_ncdt.py
```

下一份有判别力的样本是**同一台 Z8 已确认能原生出声播放并返回的整份未转封装 MOV**。优先使用可选 H.264 8-bit SDR 和原厂音轨的设置；同时记录固件、格式、帧率、方向、DCF 路径和播放结果。若该录制档使用 MP4，保留实际原厂容器和设置，并另给可播 MOV 对照。取卡读文件足够。以真正可播文件为基准，对比删改 NCDT／加定时轨，记录每次“能否列出、缩略图、进入播放、音画、退出”；这才能区分索引、metadata、解码和返回。直接复用 08 的 donor 只验证了文件开头与 metadata 解析。
