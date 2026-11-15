# v51d NCDT 与音轨顺序独立复核

复核对象是 `writer/nctg-work`，对应 `writer/nctg-changes.json` 的 6 个最终 SHA-256。生产源由 portrait Agent 修改，本复核只增加独立测试。结论：本轮已证实的两个格式阻塞均已修正；未发现新的源码或容器层面阻塞。完整原厂 UI 播放与最终固件拍摄由独立 QEMU 测试继续确认，不能由本页的低层 parser PASS 代替。

## 拒绝门槛的实际证据

1. 原厂 JPEG 中断处理函数 `0x40c0c598` 在 `0x40c0c6e4` 比较请求格式与解析出的 JPEG 格式。实际 ARM 指令运行中，422 请求与 420 文件不一致会产生 `0x2800` 并走停止路径，不启动 image 解码；422/422 和 420/420 均产生 `0x2000` 并启动 image。测试为 `test_jpeg_format_gate.py`，输出 `container-variants/jpeg-format-gate.json`。这是已执行的固件指令和受控 MMIO 测试，不是物理 JPEG 硬件验证。
2. MOV parser 在 workspace `+0xa88` 保留当前 track 类型。未知/meta trak 设置为 4；其后紧接的 udta 会被错误当作需要 handler 分类的内容，返回 6。有声音轨能把该状态更新为 2，掩盖问题。此前 v51b 真正拍摄出来的无声 `DSC_0001.MOV` 在实际 ARM parser 中返回 6；只将 timed metadata trak 移至末尾，媒体内容、偏移和总长度不变，即返回 `[0,0,82695,0,1,0]`。证据 `container-variants/silent-parser.json`。

最新 writer 将视频及可选音轨的 moov 前缀先写出，流式写 udta/NCDT，最后写 Apple root meta 和 timed metadata trak，然后回填 moov 长度。没有扩展 metadata hook 的豁免范围。

## 最终源与成功文件

`test_writer_audio_paths.py` 对真实 transaction/MOV/audio/JPEG writer 使用 ASan、UBSan、`-Werror`，共 10 路通过：正常有声、无声、部分 PCM 已写后读取失败回退，以及 frame 失败、NCTH 写失败、初始取消、音轨构造失败、NCDT 最终回填后取消、metadata 尾部写失败、moov 长度回填失败。后 7 种失败路径均删除未完成 MOV；两个取消反例的取消后写次数均为 0。源哈希与结果在 `audio-paths/transactions.json`。

4 个作者最终样本，加 3 个独立 C 生成的成功样本，全部执行实际 ARM MOV/NCDT parser：

- 整 MOV 均返回 `[0,0,110933,0,1,0]`。
- NCDT 四图均返回正确尺寸与索引：160×120、1280×720、640×360、1920×1080。
- query0/file-info 中正常有声音轨为 1，无声和部分 PCM 回退为 0；frame count 95、fps 30000/1001 与格式字段一致。时长字段为 3169 毫秒。

证据为 `audio-paths/final-parser.json`、`container-variants/file-info-candidate.json`。解析固件为 C3.11+NFP 基础及本项目已限定的 meta handler hook，并非未修改原厂固件。

## 内容、边界与兼容

`test_final_audio_integrity.py` 对正常文件全部 162287 帧验证：原 s16 双声道逐样本扩展成 `{0,低字节,高字节}`，与实际 LPCM 的 LE24 数据逐字节一致。LPCM v2 的 rate/channels/bits/flags、6 字节 stsz、24024 帧 chunk，以及 LVJB 指向同一音频范围。NCTG 的 31～34 字段与实际有声音轨一致；无声和部分读取失败的文件均没有 soun trak、LVJA/LVJB 音频引用，相关 NCTG 字段清零。部分 PCM 原始残留仅为未引用的 mdat 内容。

3 个成功路径均保留 Apple content identifier、与 MAN 一致的 UUID、9 字节 still-image-time 样本；95 帧 HEVC 每帧一个 AUD，GOP chunks 为 `[15,15,15,15,15,15,5]`，sync table 与 chunk 首帧一致。证据为 `audio-paths/*.validation.json` 和 `audio-integrity.json`。

422 缩放单独用四象限 I420 色度模式验证，四种 NCDT 图全部经 djpeg 完整解码，SOF Y sampling 为 `0x21`，16 个色块样本与预期一致；ASan/UBSan 无越界，报告 `audio-paths/chroma-sampling.json`。源色度仍按原 I420 半高寻址，只在输出 422 的目标色度高度上扩展。

保留的 PB audio_begin/preview_reader 真实源码测试再次通过 LVJA、LVJB、读取边界、2.4 秒上限、末尾清零、第二块失败、损坏长度和缓冲区哨兵，见 `pb-audio-final-v51d.json`。这证明旧 LVJA 兼容以及新 LVJB 下转换代码，不能证明真实麦克风输入或扬声器输出质量。

metadata 尾部保存在 `live_mov.atoms` 中，JPEG 使用另一块既有 I/O 缓冲区，不会覆盖待写元数据。JPEG 不进入 16 KiB atoms。`extend_moov` 对待写尾部和 32 位文件边界检查；任何尾部写入或 size 回填失败都进入 owned-file 回滚。`live_mov` 增加 8 字节必须完整重编译，不能热替换旧 workspace 的局部二进制。

本复核未操作真实相机、USB、存储卡或刷机。
