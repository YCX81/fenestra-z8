# 最终 v51 Live 拍摄与文件验证

后续独立审查发现本页记录的 v51b 无声 MOV 仍被原厂 MOV parser 拒绝：metadata trak 后的 udta 触发返回 6。本页的 ffmpeg 和文件结构检查不等于原厂播放器接受。该问题已在 v51d 将 Apple metadata 移到 moov 最后修复；见 `../writer-review/NCDT-REVIEW.md`，最终拍摄与回放应以 v51d 回归为准。

测试 MAIN：`1b0ecc754467c893e37085653f286479f7ea9341d3c10653b28ab1aca93e782b`，构建来源 `/path/to/user/v37build/v51b`。运行 `v51-final-live` 与其他 Agent 的回放测试并行，使用独立虚拟卡和 named-pipe 通信；真实菜单开启 Live，然后拍摄，等待驱动正常结束并导出文件。

结果：编码 921 帧，Live 事务成功 1 次，事务错误 0，原厂照片及 Live JPG/MOV/manifest 写出。

实际 MOV：`runs/v51-final-live/files/LIVE/100NCZ_8/DSC_0001.MOV`，890154 字节，SHA-256 `8d8aaf38875c97be4eddc13e1311a0739a3069fae84549ab133046631af3682d`。

- HEVC 1024×682，77 帧，3.294255 秒；`ffprobe -count_frames` 确认 77 帧；`ffmpeg` 全片解码返回 0，错误日志为空。
- ftyp 包含 `niko`；视频按 GOP 分块 `[15,15,15,15,15,2]`，chunk 起点与 sync sample 表一致；每帧只有一个 AUD。
- moov/udta/NCDT 的尺寸和子 box 边界完整；NCTH 160×120、NCVW 1280×720、NCM1 640×360、NCM2 1920×1080，全部有完整 JPEG SOI/EOI 和对应 SOF 尺寸。
- Apple content identifier 与 MAN 中 UUID 一致；still-image-time 的 metadata track 和 9 字节样本保留。
- 原厂 sound id 1、id 2 各调用 1 次，仅代表仿真 API 路径。

`audio_tracks=0`、`audio_skipped=1`，本轮没有麦克风录音轨；不拿这份无声 MOV 证明实际 LE24 录音。音频格式、增益与新旧 LVJA/LVJB 兼容，另由最终源码的主机测试验证（`../writer-review/pb-audio-final-v51.json`、`gain-final-v51.json`）。没有进行真机、USB、上卡或刷机操作。

结构检查可用 `../writer-review/validate_qemu_movie.py` 重放。原始追踪、最终检查点、输入哈希、输出文件及 ffprobe/ffmpeg 结果均随本目录保存。
