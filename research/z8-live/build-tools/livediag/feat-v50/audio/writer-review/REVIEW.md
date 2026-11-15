# v51d writer 独立复核

最终验收对象是 `writer/nctg-work` 中与 `writer/nctg-changes.json` 的 6 个 SHA-256 一致的源，已由主 Agent 整合并完整构建为 `/path/to/user/v37build/v51d`；执行 MAIN 的 SHA-256 为 `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`。本页保留最初相对 v49 的音频及取消修复复核；最终 NCDT、422 JPEG、无声音轨和 metadata 顺序的完整独立验收见 [NCDT-REVIEW.md](NCDT-REVIEW.md)。

结论：已发现的取消响应、NCDT JPEG 格式和无声文件 metadata 顺序问题均已修复并复测；当前没有新增阻止合入的缺陷。10 路 ASan/UBSan 事务、7 个实际 C 生成文件的原厂 ARM parser、Apple marker、完整 PCM 比较与 LVJA/LVJB 兼容测试通过。这里的结论是源码、主机测试和文件格式层面的判断，不代表真实麦克风、扬声器或原厂播放器已经通过真机测试。

## 已修复的问题

NCDT 元数据路径起初只在取中心预览帧时检查取消。四张 JPEG 编码期间 `preview_put` 没有检查取消，调用者会等四张图片全部生成后才删除文件。在首笔 NCDT 头写入后注入取消，原实现继续写 12 次，然后才回滚。

已由 writer 作者在 `preview_put` 加入 `stop()` 检查。相同反例复测后，取消后的写入是 0 次，返回失败并清理 MOV；ASan/UBSan 无异常。证据 harness 为 `cancel_ncdt.c`。

## 音频格式与容量

- transaction 每批最多 `65540 / 6 = 10923` 帧。输入 s16 双声道占 43692 字节；倒序扩展后占 65538 字节，处于同一 I/O 缓冲区内，扩展不会覆盖尚未读取的输入样本。
- 写出的每个 s24le 样本是 `{0, s16低字节, s16高字节}`；LPCM v2 描述与 `stsz=6` 一致。音频连续存储允许物理写入批次与 MOV 的 24024 帧逻辑 chunk 边界不同。
- `audio_trak` 的 `stco`、`stsc`、尾块样本数与总帧数一致；源帧数最多 192000，偏移与字节总长检查在 32 位文件界限内。
- 新预览标记 LVJB 表示 6 字节/帧；reader 使用 6 字节范围检查；PB 丢弃每个样本的低字节，恢复源 s16。旧 LVJA 仍使用 4 字节/帧直拷贝。
- 真实 `audio_begin` 的主机测试覆盖 LVJA/LVJB、0/1/32768/32769/115200/192000 帧、跨读取块、2.4 秒播放上限、尾部清零、缓冲区哨兵、第二数据块读取失败和越界声明。只替换了设备地址约束和 ARM cache 指令，读取尺寸、转换和状态代码均来自实际 PB 源；ASan/UBSan 通过。
- 最终源的增益回调测试也通过；因此此次 LE24 转换不会重新引入半块 4 倍/半块 1 倍的旧错误。

最终测试输出：`pb-audio-final-v51d.json`、`gain-final-v51d.json`；正常/静音/部分 PCM 失败回退的实际文件检查为 `audio-paths/audio-integrity.json`。增益必须用整合后的 `integration-writer/final-work/src-fix37/runtime.c`（SHA-256 `44fb23e755c99001454b76082b4ca6e460a0c163b99b349fc6089693d3c89e11`，与远端 v51d 一致）；`nctg-work` 只是 6 文件 writer 候选，其余 runtime 未集成 v50 修复，不能作为最终 runtime。旧半块增益负对照确实在 8192 帧接缝失败，最终源通过。早期 `pb-audio-final-v51.json`、`gain-final-v51.json` 仅保留历史；`test_pb_audio.py --source ... --report ...` 可重放。

## 流式 NCDT 与清理

v51d 先写 moov 的视频/可选音频前缀，流式追加完整 udta/NCDT，最后追加保留的 Apple meta 和 timed metadata trak，再更新 moov 长度。四张 JPEG 不进入 16 KiB atoms 缓冲区。新的 JPEG 读取采用源帧尺寸计算色度平面与缩放索引，四组固定目标尺寸均满足偶数/范围要求。NCDT 失败、Apple 尾部写失败、moov 长度回填失败及后续 preview 失败均进入同一文件所有权清理路径，原厂照片文件不在清理对象内。

writer 作者另修正了最后一张 preview JPEG 超限重试后残留填充的位置：LVJB 记录保持紧接索引中的最后一张 JPEG，填充移到记录之后。已复核该改法与 `lp_clip_open` 的 `c->at[n]` 读取位置一致；该作者提供的噪声末帧回归由主整合记录。

旧 `codec/audio_mov_test.c` 仍直接写 s16 raw，而新版 trak 描述是 LE24，不能把该旧样本当成新版音频格式的成功证据。本次使用真实 transaction 的新 host writer 样本及上述真实 PB 函数测试。
