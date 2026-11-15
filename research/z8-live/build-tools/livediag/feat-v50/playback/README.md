# v50 playback / stock Wave5 handoff

最终 v51d 在固定的修正 QEMU profile 中已通过完整 Live 拍摄、REC 预览和退出恢复断言，见 [A2-MODEL-AB.md](A2-MODEL-AB.md) 与 `v51d-core-regression-final.json`。修正仅启用同一 binary 已有的 secondary-stop 模型，未修改固件或放宽等待；单次对照不外推所有历史失败原因或真机结果。固定重跑入口为 `launch_validated_recovery.sh`，完整 driver 在 `validated-v51d-recovery-driver/`。

旧 profile 的集成审计、实际 Live 文件及恢复失败原记录见 [RECOVERY-RESULT.md](RECOVERY-RESULT.md)，实际 A2 分支定位见 [A2-TRIGGER.md](A2-TRIGGER.md)。这些原始失败证据继续保留。

本目录的 `v49-handoff.patch` 将 `runtime.c` 从 v49 移植到当前 v50；没有改共享 v49 源码或旧交付。`runtime.c` 是相同补丁的完整结果。原始 v46/v46r/v49 文件保留在 `remote/`，便于重放审查。

v46r 的原始两处差异保存在 `v46r-runtime.patch`。v50 进一步修复以下交接边界：

- 在 encoder preempt 之前发布 hold，阻止下一轮 Live 工作恢复；handoff 中的 hold 不会因新取景帧而提前解除。
- 由 worker 在释放 stream descriptor 及 job 之后发布 ACK；调用任务最多等 1.5 秒，不再跨任务裸读 descriptor。超时仍继续原厂调用，并留 EV_MODE=4；ACK 为 EV_MODE=3。
- ACK 后再 preempt 一次，覆盖首次 encoder init 在第一轮 preempt 之后才发布 `hw` / magic 的情况。
- 原厂 VDI owner (`0xa02d3d980`，与 encoder `open_stream` 使用同一地址) 非空时持续保持；释放后必须有更新的照片取景帧且经过一秒才恢复。此条件不依赖 0.3 秒的帧新鲜度。
- CAS 清 hold，避免覆盖并发 hook 新发布的交接状态。保留 runtime ABI，使用原有 `crumb_pad` / `vpad`。

“释放”精确指 Live stream 大块（本次源的申请值约 66 MB）及其内部视频/预览缓冲，与切到录像模式的同一 `live_stream_step` 路径一致。Wave5 自身两块持久 pool 分配、诊断分配并未因此释放。1.5 秒是释放 ACK 等待的上限，不包括原有 encoder preempt/stock close 耗时，也不保证超时场景已经释放。

`test_handoff.py` 从实际 `runtime.c` 摘取函数，只替换硬件读、时钟、RTOS delay 和 encoder mock；17 项状态/顺序断言通过，包括低帧率、stock VDI 持有、mux 尚未退出、超时、毫秒计数回绕，以及清 hold 时注入并发新 hook。

尺寸补丁为原 `liveplan.S` / `livefbh.S`，分别位于 `0x401fd000` / `0x401fd100`；原厂入口在 `0x402f3264` / `0x402f3c54`。主 Agent 在合并构建中重新编译且核对与原二进制一致。`test_caves.py` 在 Unicorn 执行实际二进制，19 项通过，结果见 `cave-test.json`。覆盖 1024×682、1280×852、范围边界、原厂尺寸/非 HEVC/其他调用者回退、856/864 编码高度，以及调用点仍需的寄存器保留；不等于真实显示效果验证。

QEMU 启动与取证：

- `launch_compare.sh`：原版 v46 与最终 v46r (16535ca8) 并行 Live 拍摄→REC 预览对照。
- `launch_final.sh`：v50 MAIN `4cb5235fc709220f64af5400680653999d2a03b0d5a4821975e0f6a7b2d50977` 的并行 Live 拍摄→REC 预览与原厂 4K 回放；执行前固定 SHA 验证。
- `summarize_qemu.py`：从 finished result.json 提取输入和导出文件 hash、错误、编码/预览帧数。

所有 QEMU 均用独立输出、卡镜像及通信管道；未访问 USB、未上卡、未刷机。原厂回放 lane 使用 rdw 模型，记录了既有 `Z8_TOLERATE=15431000:15432000`；此模型无法验证 Live 实际编码时的硬件资源竞争。没有把容忍该 MMIO 或解码出帧等同于整套 QEMU 检查通过。

## 已结束的 v50 QEMU

**单次成功不能视为稳定通过。** 后续相同 v50 的控制轮也复现“退出回放无新取景帧”；目前恢复路径未通过稳定性回归，v50 / v51 均不得因下面的单次成功而标为可交付。

- `qemu-v50-stock4k.json`：原厂播放器整条退出码 0，95 帧，零未知 MMIO；限制仍如上。
- `qemu-v50-recovery.json`：拍摄→REC 预览→半按回拍摄整条退出码 0，`trace_complete=true`、`error=null`、无失败检查；预览 45 帧，退出后 Live IRQ 1580→2074、编码帧 421→667、phase=2，最后共 715 编码帧。
- 必要截图位于 `screenshots/` 与 `screenshots/v50-recovery/`。

原 `rerun-v41` driver 的 `REC` 原厂键状态预期有误：Live 的 `pb_key_decode_wrap` 会消耗 `body[9]`，原厂 decoder 看到的 REC 位为零；旧 driver 却要求 `0x2000`，因此已经播放结束也一直等待。v46/v46r/v50 旧 driver 的超时按原样保留，未改写为通过；证据见 `driver-rec-expectation-evidence.json`。

独立修正 driver (`run_live.py`，远端 `~/fxfix/v50-playback/recovery-driver/`) 只在已确认播放照片的 cache/lookup 状态后改变该次 REC 的精确 44 字节预期，并额外要求 `consumed_rec` 恰好增加一次。所有其余键检测保持。等待预览结束后要求 frames>0、playing/error/lookup_error=0，半按退出，之后要求 Live requested=1、IRQ与编码计数增长、job/error=0。未取消导航检测、未写 guest RAM 或寄存器、未改变固件。

## v51 独立审计与回归记录

`candidate-v51-review.json` 对 MAIN `1b0ecc754467c893e37085653f286479f7ea9341d3c10653b28ab1aca93e782b` 完成独立逐字节审计：基于 v50 的 85,893 处字节差异全部落在声明允许的 Live/PB payload、hook、24 字节 metadata helper 及 build ID 内。v50 的 runtime（让路与双声道增益）、v49 字体与 Preparing、竖屏及两个尺寸补丁保持；最终 manifest 可完全重放、symbols 与实际 v51b 对应。

v51 第一轮 recovery 在编码的第一个帧返回 -24（Wave5 300 ms completion 超时），phase=LA_FAILED；没有进入 writer，随后 REC 所需 Live 文件缺失，driver 按设计报错。结果保留 `qemu-v51-recovery-first.json`，未抹去或视为通过。再次运行仍用同一固件，新增的 `handoff_debug` 仅从精确源码生成的偏移只读获取 hold / HEVC / W5 状态（`debug_offsets.c/json`）；没有延长固件时限或修改 guest 状态。

v51 r2 / r3 都成功写出 Live（transaction=1/error=0，分别编码410/417，预览38/39帧），但半按返回后 IRQ 与编码均不增长，原厂出现 `Error. Turn camera off and then on again.`。v50 control r3 同样复现（编码428，预览39）。`qemu-v51-recovery-r2.json`、`qemu-recovery-r3-compare.json` 与 `recovery-failure-evidence/` 保留原失败及截图。v50 control r2 在更早的 MCU ACK 判定失败，未到拍摄，不能用作恢复对照。

失败轮的 hold=0、HEVC open=0、W5 busy/close_failed=0、stream.error=0；1543 raw 取景控制保持 starts=3/stops=3/inactive，半按返回后没有新的 START 写入。成功 v50 的对应位置为 starts=4/stops=3/active，primary-d980 pending 被原厂 reset/cancel 清除。这个差别只定位到原厂恢复路径未继续，并未证明固件缺陷或模型缺口的根因。

输入归一化核对：v50 成功轮与 v51 r2 的 QEMU、RAW、profile、machine properties、runtime profile、ROM 输入、probe 参数相同，仅 MAIN 及其 overlay manifest SHA 不同。但成功轮尚未加入 W5 额外只读诊断，后续轮含该诊断。`prepare_noobserve_driver.py` 重建原先版本，与当前 driver 逐行相比只差7行诊断读取；`launch_noobserve_pair.sh` 并行比较 v50/v51，保留全部恢复断言，不以重复运行掩盖失败。

`error-service-disasm.txt` / `error-record-disasm.txt` 定位原厂错误记录入口 `0x41018a80`（ErrorInformationService）及其 CaptureInformationService 调用路径 `0x40ece93c`。`prepare_error_driver.py` 准备仅在这两个入口记录寄存器的现有被动 QEMU plugin point，并只在最终检查之后读取历史 `0xa03e04f44..+0xa0`；尚不对未知错误kind赋予语义。

无额外 W5 诊断的两轮 `v50-pb-noobserve` / `v51-pb-noobserve` 都复现恢复失败（分别预览38/37帧），说明额外 W5 读取不是失败必要条件；结果见 `qemu-noobserve-compare.json`。

稀疏错误观察的 `v50-pb-error-observer` 完整通过（preview45，IRQ1558→2052，编码428→675，final723，原厂错误入口零命中，历史全零）。同驱动原版 v49 对照 `v49-pb-error-baseline` 在第二次进入菜单后即记录 capture kind `0x40` / history type `0xa2`，IRQ停在446，没有Live文件；它未进入回放目标路径，不能算恢复对照。`qemu-stock-error-observation.json` 保留原厂寄存器、零覆盖/零读取错误的plugin收据及历史160B。综上 v50 恢复仍是两次成功、两次失败，不能用最新一次成功覆盖失败。

静态表 `0xa029d8280` 的条目 `0x40` 为 `[0x40,0xa2,0,0x10]`，`0xa029d88a8` 又将外部码 `0xa2` 映射到该kind；尚未闭合码的物理含义。11月4日旧记录 `../../../../_mac-migration/files/_bundle-20261102/codex-tasks/live-playback-ui/evidence/restore2-model-handoff.md` 也保留 c770 base 无 REC/preview 的同类返回失败，但其 QEMU 不同，只能作为历史线索。
