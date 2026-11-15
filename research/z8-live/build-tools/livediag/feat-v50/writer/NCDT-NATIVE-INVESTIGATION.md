# NCDT 与完整原厂播放的后续分界

主 Agent 的 `HOST_4K.MOV` 在初版 v51 writer 候选中显示 Cannot display this file、视频 decode=0。它保留 C writer 的 NCDT、音频、表和 Apple metadata，仅换成已知可解码的 4K 视频。相同固件下旧 `LIV_9013-META.MOV` 可完整解码 95 帧。故低层 MOV parser + 四张 NCDT 查询 + GOP 检查通过，仍不足以证明完整原厂播放器可用。

## 两条正交诊断样片

`ncdt_variants.py` 仅改 `HOST_4K` 的 NCDT 内容及父盒长度，mdat、三个 trak、其余 moov/udta 子项、NCHD、NCDB、尾部 free 均逐字节保留。

- `parser/native-fields.MOV`：旧 82 条 NCTG（0x22/23/101/102 改为当前实际尺寸），保持新四张 JPEG。SHA-256 `1997bea3420f7e8c6a7a8fff6cef5559f6d09e4aab3158c5ecac200dcefe5639`。
- `parser/native-jpegs.MOV`：保持新七条 NCTG（0x101/102 改为旧图片尺寸），换入旧四张 JPEG。SHA-256 `15098b39affdbf0b65baf0156ec09e285ea666f4f068bfe284bd3eb7ebd184fa`。

这些 donor 字节仅用于隔离实验，不进入生产 writer。完整运行结果由主 Agent 记录。

## 已确定的差异与消费者

旧 JPEG 为 4:2:2，SOF0 的 Y sampling 为 0x21，有 JFIF APP0，两张 DQT 和四张 DHT。新 JPEG 为 4:2:0（Y 0x22），无 APP0，将两张 DQT 合盒，Y/U/V 共用标准 Huffman 表；二者都是可在 host 解码的 baseline JPEG。后续实际 ARM 执行已经证明其中422请求与420输入的格式不匹配会阻止image启动，详见下方最终分界。

NCTG 新版仅有 1、2、3、0x22、0x23、0x101、0x102。完整差异在 `parser/ncdt-tag-diff.json`。下列消费关系来自 C3.11 指令与初始化表，不只是字段名称推断：

| tag | 解析结构偏移 | 已确认消费 |
|---|---|---|
| 0x13 | +0x6c | 帧数；0x406cec98 取值，与帧率分母相乘并换算毫秒 |
| 0x16 | +0x84 / +0x88 | 分子/分母；0x406cec90..ceccc 计算总时长；0x406c0070..01c0 映射标准 FPS 枚举，0/0 进入未知 0 |
| 0x17 | +0x8c / +0x90 | 另一帧率；0x406cecd8..cecec 与 0x16 比较并设置变速标志 |
| 0x18 | +0x94 | 输出 query0 +0x28，目前未充分命名 |
| 0x21 | +0xa0 | 原厂 writer 从格式状态 +0x24 查表填入；完整含义未确认 |
| 0x24 | +0xa8 | query0 +0x2c；原厂文件信息路径用值==2设置标志 |
| 0x25 | +0xac | query0 +0x30；文件信息按 8/10/12 分类，符合视频位深 |
| 0x26 | +0xb0 | query0 +0x34；文件信息将 1/2/3 映射为不同标志，确切命名待查 |
| 0x32 | +0xb8 | 音频声道数非零决定音频存在标志 |
| 0x33 / 0x34 | +0xbc / +0xbe | 音频位深与采样率，旧样本24 / 48000 |

`0x406ce364` 实际 ARM64 初始化执行给出上述指针（ROM 初始表 0x44dea658 → RAM 0xa0297f658，解析结构 0xa0456a820）。公开 ExifTool 文档也将 0x13 / 0x16 / 0x32..34 命名为 FrameCount / FrameRate / AudioChannels / AudioBitsPerSample / AudioSampleRate：[ExifTool Nikon NCTG](https://exiftool.sourceforge.net/TagNames/Nikon.html#NCTG)。未用外部文档补齐未知格式枚举。

生产数据有明确来源：`live_w5enc.c` 的 HEVC mode entry 固定 30000/1001、GOP 15，因此可据实记录编码 nominal FPS；帧数取 `live_mov.count`，音频当前为2声道24位48k。真实播放时长仍应以 MOV 的逐帧 PTS/stts 为准，不能用 donor 的123帧或旧拍摄日期填充当前素材。

## 最终分界与生产修复

主Agent的整NCDT替换实验通过95帧原厂解码；单独旧NCTG+新420图仍显示Cannot display，新七字段+旧422图则能显示缩略图和OK Play但未启动视频。这些对照支持NCDT图与元数据均需修正，但不能据此宣称某一个未知字段单独构成UI拒绝条件。

独立audio Agent执行了真实原厂JPEG header IRQ代码：0x40c0c598处理头中断，0x40c0c6e4比较请求格式和header低三位。请求1/输入2触发0x2800并停止，不设置image_start；1/1触发0x2000并启动image；2/2也可。所以问题是原厂播放路径要求422，而初版提供420，并非JPEG库不支持420。生产仅将NCDT四图改为动态422，Live预览420保持逐字节一致。

生产NCTG现为19条，只写当前片段可证实的数据。帧数取真实count，FPS和编码器共用30000/1001常量；codec按当前AVC/HEVC为2/3，当前8bit420 SDR profile的格式字段为1/8/1；音频只有成功写入时为PCM、2声道、24bit、48000Hz，无声与读取失败回退均为0。没有拷贝donor私有字段、日期或图像。NCTG名义时长和MOV实际VFR stts可能不同，真机声画同步仍待验证。

另一个无音轨专有失败已闭合：timed metadata后面接udta使原厂parser沿用metadata track kind并返回6。transaction现在先写video/成功的audio，再流式NCDT/udta，最后追加原本的小Apple root metadata和timed meta轨，并回填moov长度。未扩大原厂meta hook接受范围。最终四类样片和独立生成的三类样片共7个，实际C3.11 ARM parser全部返回[0,0,110933,0,1,0]，四图查询成功；有声、无声、部分PCM后回退的file-info音频标志正确。证据见../audio/writer-review/audio-paths/final-parser.json。

最终v51d完整原尺寸测试由主Agent执行：目前已通过原厂UI、缩略图、MOV解析、DEC_SEQUENCE 1024×682、帧缓冲并解出第一帧，然后停在显示模型未实现地址0x15421104；这与历史原厂1080p测试的模型缺口相同。这足以区别此前拒播，但不是原尺寸完整播放或真机播放通过。4K替换fixture保留writer容器/音频/metadata，能隔离显示模型差异，也不能替代原尺寸真实拍摄片的全流程证明。最终整机报告以主Agent归档为准。
