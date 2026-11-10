# Live MOV 原厂回放实验

**本轮 HEVC 实验**另有两份 3.003 秒、1280×852 的有声测试图样片，覆盖现有 Live writer 的实验性 HEVC 封装、固件的文件打开和参数集提取。同期还复演了 ch2 地址登记与提交的条件。原来的 00–09 仍是来自同一实拍片的 AVC 对照组。[样片、复现与 C3.11 结果](HEVC-852-FLOW.md) · [Wave5 编码参数与环形缓冲](HEVC-ENCODER.md)

这个目录有一份实机 Live 样片和 10 个可复现的文件实验件。实验件已在电脑上完整解码，Z8 的列表、缩略图、播放和退出均待实机验证。保留现有 LVP 与回放路径，固件改动见[实施方案](firmware-plan.md)。1280×852 的采集来源、编码和保存限制见[分辨率调查](LIVE-1280x852.md)。此次固件解析器实跑结果见 [NCDT 追踪](FIRMWARE-NCDT.md)。

## 操作和对照关系

在本目录执行。准备脚本读取归档里的完整 donor，检查其 SHA-256 后生成 [00–09 文件矩阵](tests/MATRIX.md) 与[结构清单](tests/current-manifest.json)。源文件原样保留；若同名实验件字节有差异，脚本停止。

```sh
python3 -B prepare_tests.py
python3 -B mov_fix.py live-sample.MOV --inspect
python3 -B mov_fix.py live-sample.MOV tests/manual-niko.MOV --niko
python3 -B mov_fix.py tests/07-all-four.MOV tests/manual-ncdt.MOV \
    --ncdt-from ../../../_mac-migration/live-session/live-photo-20261030/pair-u02a/DSC_0002.MOV
python3 -B mov_fix.py tests/06-niko-no-mebx-stco.MOV tests/manual-crop.MOV --crop-to-container
```

单项开关为 `--niko`、`--remove-mebx`、`--audio-stco`、`--size-from-sps`；`--all` 同时启用这四项。`--ncdt-from` 独立指定 donor。02–04 都以 00 为对照；遇到首个 `niko` 检查即可失败。逐级隔离后续影响要比较 **00 → 01 → 05 → 06 → 07 → 08**。保留 682 行显示区域时，另比 **06 → 09**。长文件名仅用于电脑端标记。

| 改动 | 改写范围 |
|---|---|
| niko | ftyp 从 20 扩到 24 字节，在文件偏移 20 放置第二个 compatible brand `niko`。重算各保留轨道引用的 mdat chunk 地址。 |
| 删除 mebx | 移除 handler=meta、sample entry=mebx 的整条定时轨及其 edit list。moov 级 UUID 保留；mdat 内原 9 字节成为未引用数据。 |
| 音频偏移表 | 音频 co64 转为单项 32-bit stco，保留原 sowt PCM、时间轴与采样数。 |
| SPS 尺寸 | 本件把 avc1 和 tkhd 从 1024×682 改为 1024×688，校正对应旋转平移。没有重新采样、放大或写出 1280×852 像素。 |
| NCDT | 08 仅比 07 多出一个 324,169 字节的 moov/udta，其中是 donor 原有 324,161 字节 NCDT。移走这一个 udta 并恢复 moov 长度后，逐字节等于 07。 |
| SPS 裁剪 | 09 相对于 06 将 SPS 底部 crop 从无改为 3 个单位（6 行）。编码矩形仍为 1024×688，SPS 显示尺寸、avc1 与 tkhd 均为 1024×682。avcC 多一字节，其它 SPS 位（包括 VUI）、PPS、mdat 均保留。 |

工具输入限定为本 Live writer 的自包含、非 fragmented AVC MOV。它按 mdat 实际位置重算 stco/co64，保留未知盒字节；私有内部地址和其它 MOV 变体需要单独处理。donor 只提取唯一的直接子盒 `moov/udta/NCDT`，检查 `ftyp`、`NCHD`、四个必需子盒、图像头与长度、子盒边界。重复注入同一个块不会改动结果，已有不同 NCDT 或已有不含 NCDT 的私有 udta 时会报错。上述校验不能替代固件的 NCTG 解释、图像解码或逐模式兼容性测试。

`--crop-to-container` 和 `--size-from-sps` 互斥。裁剪实验要求逐行 4:2:0、单一 avcC SPS、流内无 SPS、容器宽度等于编码宽度，目标高度只能去掉最后一个宏块内的偶数行。当输入已含不同裁剪、要求放大或需要重新采样时会报错。

## 实机样片与模拟 donor

00 与项目中 `live-export/DSC_9776/DSC_9776.MOV` 字节相同。其 86 个 H.264 视频 sample 含 1024×688 的 SPS，无裁剪；容器声明 1024×682。电脑播放器可以采用容器的显示区域。

08 的归档 donor 是 `pair-u02a/DSC_0002.MOV`，由历史 QEMU 录制链生成，含旧 Live 元数据。ffprobe 识别为 **3840×2160 HEVC、24-bit LPCM、105 帧、mebx**。08 实际保留 Live 样片的 **1024×688 H.264、16-bit PCM、86 帧**，并携带 donor 的字段与预览图。它可供检验「加入这一整块是否改变表现」；用它显示出的 donor 缩略图不能证明 Live 自身的缩略图正确。字段及固件入口见 [NCDT 实验](FIRMWARE-NCDT.md)，早期反汇编见 [NCDT 结构分析](NCDT-analysis.md)。

## 离线验证

10 件的 mdat SHA-256 相同，各保留轨道的 sample payload 哈希相同。逐件重算了 sample 计数、边界、时序和 AVC NAL 长度，覆盖原四项的 16 种组合、幂等与零改动。生成器还校验源 donor、转植的字节范围，并拒绝 66 字节的空结构探针作为 donor。原 00–07 的归档和当时的验证记录仍在 `tests/manifest.json`、`tests/validation.txt`。

[独立解码记录](tests/decoded.json)来自 ffprobe n5.1.2 和 ffmpeg 4.3.1。本机从已安装应用中调用这两个命令行程序，全程使用本地文件。`prepare_tests.py` 会先在 PATH 中查找，再尝试本机附带的工具。单独运行 `verify_media.py` 可获取完整 JSON，也可显式指定路径。

```sh
python3 -B verify_media.py \
    --ffprobe '/Applications/Parallels Toolbox.app/Contents/Frameworks/ToolboxCommon.framework/Versions/A/Resources/ToolboxCommon.bundle/Contents/MacOS/ffmpeg/ffprobe' \
    --ffmpeg '/Applications/Downie 4.app/Contents/Resources/ffmpeg'
```

- 每件解出 86 帧，PCM 均为 48 kHz、双声道、156,717 个采样帧。解码后的 PCM SHA-256 一致。
- 禁用自动旋转，00、01、02、03、05、06、09 的完整解码画幅为 1024×682。04、07、08 为 1024×688；从左上取相同的 1024×682 后，逐帧拼接的像素 SHA-256 与 00 一致。新增可见的六行来自原有编码区域。
- 00 的 MOV content identifier 与配对 JPEG 的 Apple MakerNote 0x0011 一致。按本样片的 1,000,000 Hz movie timescale，此 mebx 标记从 **1.735084 秒**开始，持续 **0.144655 秒**。删轨件保留同一 UUID，但播放器无法再从该轨取 still-image-time。当前导出可使用现成的 JPEG/MOV 原始配对；未来 camera-only 格式把 UUID、标记的准确开始/时长和 timescale 保存到小 manifest，导出时独立重封装 Apple MOV，并在 iPhone 上验证导入。

离线运行当前 MAIN 的低层 MOV 解析入口并提供文件字节时，00–05 返回 6，06–09 返回 0。06、07 和 09 查询 udta 返回 3，08 查询/读取返回 0、NCTG 状态为 0。这些返回值描述各自入口，实机 UI、播放解码和声音仍待验证。[实跑记录](FIRMWARE-NCDT.md)

## 下一轮卡片顺序

1. 先取由这台 Z8 实际录制、原卡可列出、有声播完并可退出的短 MOV。记录固件版本、录制格式和设置；在实验卡的有效 DCF 目录确认这个正对照仍能列出和播放。归档 QEMU 文件不能充当实机正对照。
2. 每个实验件分配该目录内所有扩展名都未占用的 0001–9999 四位编号，使用合法四字符前缀。例如 `DCIM/100NCZ_8/LIV_9001.MOV` 仅在 9001 空闲时可用。关掉 Live，独立读卡器放件，逐轮冷启动以排除先前 Live/USB 状态影响。
3. 以 00 记录原始表现。若需要独立筛查，依次比较 01、02、03、04 对 00；若验证后续门槛，依次比较 01、05、06、07、08 的相邻件，再比较 06 与 09（SPS 裁剪）。逐件记录「列出、缩略图/信息、开始、有声播完、停止/返回」和错误文字。所有失败若停在同一可见阶段，单靠 UI 无法定位内部地址。
4. 得到实机 donor 后可用同一 `--ncdt-from` 对 07 做独立复本，与 08 和 07 比较。再从原厂可播放控制件逐项构造移除子盒等反向实验；这些反向实验件尚未生成。对素材生成正确的 NCTG/缩略图、建立 still → stock 播放入口、验证 Live 用过后的资源归还和退出恢复，均是删 LVP 前的独立验收项。
