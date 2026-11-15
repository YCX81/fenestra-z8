# v52：原厂 MOV 计时与 CFR 修复候选

2026-11-14。未操作 USB、相机或物理卡；最终整机 QEMU 与固件链接由 root 负责。

## 已复现的原因

用户所指 9044 对应 v51d QEMU 原始 DSC_0001.MOV，SHA-256 `5bbc309ce4363e4d0040a2de0add71ddafca0d386cdf3324b7c298979da006d7`。77 帧、1024×682、无音轨。视频 `mdhd` 为 1,000,000 Hz；真实 `stts` 总时长 3,298,696 µs，首帧却为 101,626 µs。

实际固件指令链（v51d MAIN SHA `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`）：

- `40cea5d8` 只读取首个 `stts` delta → parser `+a4c`，以及视频 timescale → `+a50`。
- `40ce7dc0` 导出两值；`403442cc` 取得后，打开成功回调 `40345b30/34` 放到消息 `+14/+18`；`40cfc090` 将消息复制到 `a0319f270`。
- 原厂帧时间路径 `40d03080..94` 计算 `frameIndex × 1000 × firstDelta / timescale`。独立同式 API `40cf4114` 通过原厂 getter 读取这两值。
- 播放参数由 `40cfc830 → 40cf9258 → 40cf92d8 → 40d02bec` 选择帧率档位。它只识别一组固定有理数；任何其他数值默认 enum 2（30000/1001）。因此原片用近 29.97 fps 的档位播放，但显示按每帧 0.101626 秒累加。
- `test_native_timing.py` 在 QEMU virt 上运行真实 MOV 解析器、getter、档位选择与时间换算函数：第 77 帧输出 **7825 ms**，首 delta/timescale 为 **101626/1000000**，档位为 **2**。打开回调仅将 parser 导出的两值搬到对应全局字段，由测试驱动模拟该复制；没有模拟显示器或播放调度。这个结果直接解释“约两秒却走到八秒”，没有把 NCTG 名义时长当成根因。

证据：`input-inspect.json`、`fps-parser-disasm.txt`、`player-consumers-disasm.txt`、`native-info-xrefs.txt`、`native-schedule-disasm.txt`、`native-timing-test.json`。

## 源码修复

只改 `work/src-fix37/capture/live_stream.c`、`live_stream.h`、`codec/live_mov.c`，见 `cfr.patch` 和 `changed-files.json`。不改原厂播放器调度。

1. 编码前用绝对时间网格 `epoch + floor(index × 1001000 / 30)` 调度，每次 worker 最多送一帧，避免循环里长时间占着资源。到期而无更新的有效取景图时，把缓存 I420 **重新送编码器**，从不复制已有 P 帧码流。新源时间晚于待补 tick 时，先用之前的有效图补齐；不会把未来图像填到过去。
2. 在现有单块 stream 分配里加一张 I420 临时图。只有拷贝后 lease、epoch 与撕裂检查都成功才交换有效/临时指针；失败或取消仍保留原有效图。
3. 预录中断超过 1 秒、mux 结束均重建时间网格和预录，避免陈旧画面跨段。正在拍摄的短暂空窗用最后有效图补齐。编码器 BUSY 不消耗 tick；若到 post-roll 结束后 1 秒仍追不上，取消该次 clip，不输出伪称 CFR 的短片。
4. post-roll 终点向上对齐到下一个完整帧边界，最多多一帧。`clip_start/clip_end` 仍是真实时钟，原有音频冻结直接使用同一窗口；照片时间不改。Apple marker 继续选择快门之前对应帧。
5. writer 逐个检查输入时间与最后边界均符合该网格（允许整数微秒量化不足 1 µs 的差异）才写 `mdhd=30000`、单条 `stts=(count,1001)`。其他 VFR 输入原样保留。movie/Apple 时间仍采用微秒。旧片的时间表不会被修改；9044 的显示需重拍新片验证。

新增 `live_stream` 尾部 48 B。root 必须重生成 runtime offsets，并调整 `live_w5enc.c` 内基于 runtime 大小/`w5_close_lock` 偏移的静态断言。与 handoff 的 `live_stream_release_aux()` 是独立小块，应保留该函数和声明。

真实 W5 backend 为 persist 16 B、scratch 1,048,640 B。1024×682 的新增 I420 为 1,047,552 B，stream 单块 requested 从 66,094,344 增为 **67,141,896 B**，不另申请 pool 单元。EVF 1280×852 源仍缩到 1024×682。`-Os`、完整 freestanding/strict-align flags 的已分配 object sections 总增量 **868 B**（stream 528、MOV 340）；最终链接/对齐空间由 root 核对。

## 已完成验证

- `test_cfr_stream.c` 编译实际 `live_stream.c/live_capture.c/au_ring.c`，ASan/UBSan PASS。仅池服务、取景源与编码器用可记录 stub：108001 帧、约一小时无累计漂移；源图时间不晚于输出 tick；缺帧重复送编码器；失败拷贝与取消保留有效缓存；BUSY 重试无洞；post-roll 失败不产不完整片；mux/长空窗重建；EVF 两张图在同一分配内、16 字节对齐、互不重叠。见 `cfr-stream-test.txt`。它不能证明硬件编码吞吐量，整机 QEMU 需覆盖这点。
- 实际 C transaction + writer，ASan/UBSan 输出 `cfr-audio.MOV` 与 `cfr-silent.MOV`。95 帧；音轨 152152 帧/48000 Hz，与 `95×1001/30000` 有理时长完全一致。所有 PCM 经 s16→LE24→s16 逐字节一致，GOP/AUD/VCL、四张动态 422 JPEG、LVJB、Apple UUID/9-byte marker 保留。
- Apple marker：start 1468133 µs、duration 33367 µs，对应快门之前的第 44 个零基帧。源快门 2500000 µs，clip 起点 1000000 µs。见 `cfr-host-validation.json`。
- 原厂真实 ARM 对有声/无声候选均解析成功，导出 **1001/30000**，档位 enum 2；第 95 帧 **3169 ms**，见 `cfr-native-audio.json`、`cfr-native-silent.json`。
- 旧 VFR 输入用新 writer 重建后的 MOV 与 v51d final host-sample **逐字节一致**，SHA `84d3f5d81d42c91973467e8a2f0b3a904338f5f7fd421b6dbca33b5e9960bb39`。

Host CFR 媒体是已有真实 HEVC AUs 在测试驱动里放上 CFR 时间网格，供验证 writer/container；生产重新编码机制由前述状态机和下一步真实 Wave5 QEMU 验证，不能把 host fixture 当成已验证的真实拍摄产物。没有宣称相机新版本已验证音画同步或持续编码性能。
