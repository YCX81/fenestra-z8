发现 **1 项 P1、1 项 P2 缺陷**。

审查对象：`linked-lamp-v8-ready`，MAIN SHA256：
`934164d3e6638e0c7f19512fbee2fdc3f81216eb7606e757fd280df45f070d70`。ELF 加载节与 MAIN 一致；三个修改的 C 单元经 stdout 复编，代码及重定位与对象一致。以下反例执行实际 AArch64 机器码，原生服务、时钟及调度使用受控输入，不是相机观测。

1. **P1：同步 P 发码持续阻塞 Live worker，返回照片模式后仍影响正常 Live 拍摄。**

   [worker 报告调用](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/runtime.c:238)与 `live_stream_step`、音频处理、UI 轮询在同一任务。它同步进入 [fr_worker_tick](/path/to/Nikon/build-tools/livediag/feat-rec/src-fix35/recorder/recorder.c:215)，直到整帧发送结束才返回。

   对象中 `0x401e3d14` 调用报告器，后者在 `0x401f65a4` 同步调用 `emit_frame`。执行真实报告器及发码器，设置会话已经 `RETURNED/DONE`，仍得到：

   - 一帧占用 **1,250,000 µs**；
   - 调用 **1,250 次 DELAY(1)**；
   - 完成整帧后才返回 worker。

   **触发场景：**本次 REC 会话领取并完成 I 后，保持 arm，再回到照片模式使用 Live。报告器不检查照片模式、Live 工作状态或会话结束标记，仍每约 2.5 秒发送一帧。DELAY 会让出 CPU，但不会让这个 worker 执行下一次帧处理。

   [帧处理代码](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/capture/live_stream.c:185)只处理最新 IRQ generation，并把跨过的 generation 计为丢帧。因此会反复制造约 **1.25 秒的 Live 帧处理空窗**，不能满足“照片模式下 Live 正常拍摄不受影响”。未领取会话时不触发此问题。

2. **P2：不同阻塞步骤发布相同进度，无法区分 TRIGGER 卡住还是后续 DELAY 卡住。**

   [关闭路径](/path/to/Nikon/build-tools/livediag/feat-rec/recorder-build/codec/live_w5enc.c:542)在 `w5_diag(11)` 返回后发布 `23/0`，随后调用首次 TRIGGER；进入输出等待循环、调用 DELAY 前又发布同一个 `23/0`。

   两个独立对象反例：

   | 停止位置 | RAM packed | worker 报告 |
   |---|---:|---|
   | 原生 TRIGGER `0x40c33fb0` 不返回 | 184 | P=23，detail=0 |
   | TRIGGER 已返回，后续 DELAY `0x40187b00` 不返回 | 184 | P=23，detail=0 |

   两种情况下 worker 都能继续递增心跳，光学报告完全相同。RAM `updates` 虽可能不同，但不进入报告帧。因而即使收到连续有效 P，也无法区分“原生 TRIGGER 未返回”和“已进入后续等待、DELAY 未返回”。README 已承认这种歧义，但实现仍未满足要求的步骤区分能力。

补充核验：1,680 条门控路径、576 条现场恢复／重放路径通过；三处未 armed 门控仍为 **111 指令／32 字节**。v8 进度、归属及报告门控测试和 11 项解码测试通过，未发现新增越界写或必然不开机证据。删除的是诊断发码等待，关闭路径原有 DELAY 仍保留。GPIO 叶函数及 DELAY 上下文检查不能证明原厂 REC 后一定继续调度 worker；该未知未列为缺陷。已知 `verify_link` CFG 不完整不计入本次发现。

全程未修改文件，未使用 USB、刷写、相机或 QEMU。

**判决：不接受 v8 上机。**


[exited with code 0]
