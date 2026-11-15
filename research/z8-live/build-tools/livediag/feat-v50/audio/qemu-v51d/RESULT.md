# v51d 最终 Live 拍摄与文件验证

执行 MAIN：`aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`，来源 `/path/to/user/v37build/v51d`。独立 lane `/path/to/user/fxfix/v51d-audio/runs/v51d-final-live` 与原厂回放测试并行，使用独立虚拟卡和 named pipes。driver 正常结束，退出码 0；最终编码 911 帧、Live 事务成功 1、事务错误 0。

没有复用旧 ABI 表：用 v51d 源和 `clang --target=aarch64-none-elf` 编译 offsets.o，从 ELF `.rodata` 提取所有 offsetof。`live_mov` 的 8 字节增量不改变独立分配的 runtime 结构，最终 `sizeof(live_runtime)=23184`；编译来源和头文件/最终 payload ELF 哈希保存在 `offset-provenance.json`。

实际文件 `runs/v51d-final-live/files/LIVE/100NCZ_8/DSC_0001.MOV` 为 912118 字节，SHA-256：

`5bbc309ce4363e4d0040a2de0add71ddafca0d386cdf3324b7c298979da006d7`

Live 事务结束后曾提前导出给并行原厂回放测试；完整 lane 结束后再次导出的文件逐字节相同。

已验证：

- HEVC 1024×682，77 帧；ffprobe 时长 3.298696 秒。ffmpeg 完整解码退出 0，错误日志为空。
- ftyp 包含 `niko`；GOP chunks `[15,15,15,15,15,2]`，chunk 首帧与 sync table 一致；每帧恰好一个 AUD。
- 整 MOV 经实际 ARM parser 返回 `[0,0,108193,0,1,0]`；NCDT 四图尺寸和索引全过。四张 JPEG 的 Y/U/V sampling 均为 `[0x21,0x11,0x11]`，即 422。
- moov 中 Apple root meta 和 timed metadata trak 在 udta 之后；Apple UUID `48DC1B01-5BAA-9824-AB8D-F80CBDBAB995` 在实际 JPG、MOV、MAN 三者一致；9 字节 still-image-time 样本保留。
- audio_tracks=0、audio_skipped=1、audio_error=0。本 QEMU lane 没有录音轨，不能证明真实麦克风或扬声器效果。音频增益、LE24、LVJB/旧 LVJA 兼容见独立源码/主机测试报告。

NCTG 的时长来自 77 帧除以名义 30000/1001 fps，query0 为 2569 毫秒；实际 MOV 使用输入 PTS 为 3299 毫秒。QEMU 帧间隔与名义编码 fps 不完全一致，因此这里只以 MOV/ffprobe 报告实际时长，不把二者等同。

低层 parser 通过不替代整机原厂播放器 UI 结果。该实际片已经提供给主 Agent 做独立原厂回放测试；其报告需单独说明显示模型的已知边界。本轮未使用真机、USB、上卡或刷机。

主要证据：`v51d-summary.json`、`movie-validation.json`、`actual-parser.json`、`actual-file-info.json`、`v51d-decode-result.json`、`runs/v51d-final-live/final-checkpoint.json`、`runs/v51d-final-live/manifest.json`。
