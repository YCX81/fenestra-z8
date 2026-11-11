# v37 工作快照：2026-11-11

从 WSL `~/fen-try` 的 `d460dfb` 基线复制 v37 专属未提交文件到独立工作树；原工作区未改动。此分支保存进行中的更新，不表示已完成硬件验收。

## 当前包含

- `src-fix37/`、`pb-fix37/`：单个 Live MOV、MOV 尾部 JPEG 预览、旧 LVP 兼容、图标重绘/恢复，以及后续加入的预览 PCM 音频路径。
- 预览解析和 JPEG 解码放在 Live 层，由回放层通过链接符号调用，腾出回放层空间。
- `tools-v37/`：构建和精确替换脚本；`tests-v37/`：主机预览测试、QEMU 驱动和只读静态分析工具。
- 音频代码从 MOV 预览中的 `LVJA` 记录定位 PCM，尝试调用原厂音频服务；`V37_PB_AUDIO_TEST` / `PB_AUDIO_TEST` 是专用于仿真的静音协议测试开关，常规构建不启用。

## 本次独立工作树验证

在 WSL Ubuntu 中，对本工作树运行现有 `tests-v37/run_preview.sh`，ASan/UBSan 测试通过。两组 45 帧编码、解析和解码往返成功，与 ffmpeg 的抽样亮度最大差均为 1；4 个写入失败注入点均返回错误。该测试验证预览数据及音频定位记录，不验证原厂音频服务或扬声器。

用已有 `~/fs-out` 的 v36 重建输入和 clang 21.1.8 重建当前源码，构建成功：

```text
MAIN bytes: 81807709
MAIN SHA-256: 191e6e4fc73d68c3f20a36a5e5afaf7830fb071f3074d69ec98a9f3de4924b34
Live payload: 90008 bytes; remaining: 24680 bytes
Playback payload: 34872 bytes; remaining: 6088 bytes
```

构建收据见 [verification-v37/build.json](verification-v37/build.json)。此次没有从原厂包重新构建 v36 输入，也没有生成更新容器。固件、私有输入和二进制产物未加入提交。

## 既有 QEMU 记录的实际范围

本次读取了 Claude 工作环境中的 `au-n1`、`au-n2`、`au-t1`、`au-t2` 结果，没有重跑 QEMU。这些测试使用软件编码器仿真镜像，不能证明 Wave5 实机路径。

- `au-n1`、`au-t1` 完成：各有一次事务，REC 两次累计显示 90 帧，解码错误 0。
- `au-n2`、`au-t2` 的摘要标为 `TRACEBACK`，不能记为通过。
- 两个完成用例的 `audio_plays=0`、`audio_errors=0`；没有证明音频服务开始播放，不能据此声称机内预览已有声音。
- Claude 旧 `~/v37-out` 的 MAIN 哈希为 `e86c7c4a401cd5680249316426f37b56c133a6326e01f5238e7aae6b9574fe71`。其构建副本 `pb-fix37/runtime.c` 与当前源码不同，因此旧二进制和旧 QEMU 记录不能作为本快照逐字节一致的回归证据。

## 未完成

预览音频需要继续排查和验证。当前快照尚未执行对应 MAIN 的完整 QEMU 回归；真机图标恢复、扬声器输出、性能、Wave5 与新写卡路径组合以及 Mac/iPhone 识别均未验证。较早无声版本的细节和历史记录保留在 `RELEASE-v37.md`。
