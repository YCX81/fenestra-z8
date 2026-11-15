# v52 Live / 原厂播放资源交接候选

基线为最终 v51d：MAIN `aa6e94e0042fb445c666de73c0f6b9e7e569010b7993a3ac3da0979b174a9433`、BIN `c30031834f34ee7c25646502e775293aed99ecfd6358fae09e398a02e1ccf280`。候选源码只在本目录 `work/` 修改，未更改旧交付或操作相机。

## 真机观察与结论范围

用户刷 v51 后确认 Live Off 时 9041～9044 均能原厂播放，9041～9043 有声音；Live On 时四片一开始播放均沙漏。图标正常，REC 预览刚拍的 Live 无声。当前没有新增硬件日志。

这支持优先调查 Live 占用的共享资源；它不能单独证明是内存、麦克风、Wave5 关闭还是调用时序中的哪一个因素。旧 QEMU 的分开 Live/REC 与原厂播放通过没有证明同一会话里已经编码的 Live 能在真机上交出资源。此次候选修复下述源码确定的缺口，**没有声称真机问题已解决**。

## 源码及反汇编证据

1. v51 worker 在 `live_stream_step` 后立即发布 ACK，之后才执行 `live_audio_step`。音频 want 没有检查 hold；因此 ACK 可以发生在麦克风仍 active，hold 期间也可能继续开麦。
2. 旧 ACK 只检查视频 descriptor/job，没有检查音频 descriptor、停止状态或独立的 `fr_desc` 诊断池。
3. 旧 hold 只由 Wave5 bring-up 入口触发；REC 预览不走该入口，照片回放仍处于 photo mode。仅靠近期 LV IRQ 判定不能稳定表达回放占用。
4. `live_w5enc.c` 明确保留两块持久 photo pool（arena 与包含 W5 state、buffers、slots 的 block）；旧 preempt 关闭实例但不释放这些分配。stream 的 `fr_desc` 也独立常驻。
5. 反汇编直接调用点为 MOVS `40cf9854` 与录像 `40d07ba4`，均调用已 hook 的 `402f2d58`。该入口自己的底层 W5 初始化/锁操作位于 hook 之后，所以没有证明入口遗漏或已持有 W5 内部锁死锁；调用方更外层时序仍需实际观察。证据在 `stock-handoff-disasm.txt`。
6. native view getter `40393f6c` 是 `ldr w0,[x0,#0x54]`；getter `40393f28` 读取 `+0xd0`。现有 PB profile 使用 current-view 指针 `a032280c8` 和 view ID 2。候选沿用此识别，不借 page 1495 或未经验证的编号推断。

## 候选行为

- Worker 看到 native playback view 2 即提前 hold；整个回放 view 生命周期保持。REC 与原厂播放均可使音频停止，不需要先等 W5 bring-up。
- hold 参与音频 want。音频停止完成后才考虑 ACK；完整条件是视频 descriptor/job 释放、诊断 fr block 释放、音频 active/descriptor 为 0，backend phase 为 IDLE。
- 音频候选由 audio Agent 提供：停止接口非阻塞重试，以原厂 stop callback、service idle 与 callback drain 同时确认；未确认时保留 active/descriptor。runtime 只对 `active && cancelled` 的残留失败继续 stop，避免正常停止后的 cancelled 标记永久阻止下次启动。
- Worker ACK 后，stock caller 再检查/关闭 Wave5，只有 magic 有效、无 busy/release_req、无 close_failed、实例不 alive 且 client 4 无已用 slot 时才释放两块持久 pool。先清发布的 `e->hw`，复制两个 descriptor，最后释放包含 state 自身的 block；清除已失效 persist/scratch 指针。关闭失败时保持分配，不以释放内存掩盖硬件仍使用它。
- 整个 stock handoff 使用 cached runtime 的原空闲 `lv_regs_pad` 槽作为 `stock_handoff_lock`，防止两个 caller 中一个还持有 W5 指针时另一个释放其内存。Live 自己的 client 4 路径不等待此锁。
- 修补分配失败清理：descriptor 无有效 record/容量不足、第二块分配失败、地址布局验证失败时，归还尚未提交给硬件的块。

## 明确保留的限制

**这不是无条件交接保证。** 两个已有 forwarding 语义仍在：

- handoff exclusion 等待 6 秒仍未取得锁时，记录 `EV_MODE=6` 并返回；汇编 wrapper 仍继续原厂入口。
- worker 完整 ACK 等待 1.5 秒。音频异步停止、mux 或 stream 释放若未在此内完成，记录 `EV_MODE=4`，不释放 W5 pool，仍继续原厂入口。

回放 view 的提前停车和 REC worker 等待改善可用窗口，但不能把这些超时后的 forwarding 描述为已安全让路。候选不增加对 native bring-up 返回值的假成功，不修改 EOS 或帧编码 deadline。资源实际关闭和恢复仍需要真机结果；QEMU allocator、音频与并发时序都存在模型边界。

`EV_MODE` 值 3 现在表示 worker ACK 且 W5 pool dispose 成功，值 4 表示未完成。`b` 高 32 位是 dispose 拒绝原因：1 magic、2 close_failed、3 busy/release_req、4 alive、5 client-map/slot、6 descriptor、7 未获得完整 ACK；低位记录 hold，bit8 音频 active、bit9 音频 descriptor，bit16 起为 backend phase。值 5 表示 playback view 提前 hold。没有添加卡 I/O 到入口 hook。

## 已验证

- `test_handoff.py` 直接提取实际 runtime handoff/worker 片段、stream aux release 和 W5 dispose 的 C 函数，仅 mock 原厂 API、状态和时钟。
- ASan + UBSan：36 项 runtime/worker 状态与顺序检查、12 项 W5 pool 安全释放检查全部通过。覆盖连续两个 stock caller、hold 期间不重开、音频超过 1.5 秒不假 ACK、不释放池、6 秒锁超时、mux 在读时不释放、失败 close 保留、self-hosted state 最后释放、重复 dispose、不新帧不恢复及 CAS 并发边界。
- 纳入 audio 候选六文件仅作集成编译后，完整 20 个 Live 对象通过 AArch64 `-Wall -Wextra -Werror`；完整 link 严格检查 `401fd000` 边界，通过。
- Live payload 为 **109584 字节**，较 v51d 增加 **1448 字节**；到尺寸补丁 cave 剩 **1008 字节**，未包含 timing Agent 后续 CFR 改动。`build-live-result.json` 保存结果。

本编译验证使用 audio 新结构，所以 `sizeof(live_runtime)=23216`、`w5_close_lock` offset 18940。timing 若再增加 stream 字段，root 必须从实际合并源重新核对这些断言与 offsets；不能复制旧 offset。

## 集成文件

`handoff.patch` / `changed-files.json` 只包含以下六个独占文件相对 v51d 的差异：

```text
src-fix37/runtime.c
src-fix37/runtime.h
src-fix37/codec/live_w5enc.c
src-fix37/codec/live_codec.h
src-fix37/capture/live_stream.c
src-fix37/capture/live_stream.h
```

`audio-test-dependency.json` 明确列出私有 work 中用于编译的音频依赖，**不重复包含在 handoff patch 内**。与 timing 的 `live_stream.c/h` 交集只是新增独立 `live_stream_release_aux` 函数及声明；timing 可以照常修改原 release()/allocate()/CFR 字段，由 root 按 patch 合并。root 负责最终完整 MAIN compose、独立 native 路径及合并候选 QEMU；本目录没有发布新固件。
