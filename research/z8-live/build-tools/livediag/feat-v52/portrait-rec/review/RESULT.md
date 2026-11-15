# v52 retained-owner / HEVC close 独立复核

结论：本次冻结的 3 个源文件未发现新增的生命周期、并发或实例归属阻塞，可以进入集成 QEMU 验证。

审查快照位于本目录 `work/`，SHA 与 `handoff-retained/changed-files.json` 一致：runtime `4f2d6992...`、live_w5enc `03b72ed2...`、live_codec.h `6ab78ea8...`。生产源码未由此 review 修改。

## 生命周期证据

- 普通照片 REC 预览只释放 worker 的 capture、诊断、音频缓冲，继续保留独立 W5 pool。恢复前检查全部 worker storage quiet，再持 `stock_handoff_lock` 验证 retained owner，CAS hold1→0，因而不会在 stock hook 释放池时读悬空 pool。
- retained helper 不产生非栈写入；同时要求 magic、close_failed=0、busy=0、release_req=0、VDI 固定状态、client4 对应自身 slot、其余 client0～3未映射、另一 slot 空、WSTATE=4、原 arena、原 RUN instance 及正确 codec 类型。验证失败继续 hold，不尝试偷用或重开他人的实例。
- 原厂 codec caller 仍走 pending→worker/audio ACK→第二次 preempt→dispose。第二次 preempt 必须保留，因为 worker 可能在读取旧 hold 后才完成 encoder 初始发布；第一次 preempt 无法涵盖该窗口。
- dispose 在 clean native close 后才释放两块 pool，先清公开 hw 指针，再释放描述符；close_failed/in-flight/release pending/alive 均拒绝。
- 新 close guard 除 HEVC 类型1 / AVC类型3外，还验证 RUN instance 指针和 arena。任何不一致均在首次 SUBMIT/STOP 前返回 stage1；不因此清除 close_failed。
- 如果外部 reset 导致 owner 检查失败，方案会保守保持等待；这里不承诺所有外部异常都能自动恢复。

## 独立执行

- `test_handoff.py` 从本目录实际源码抽取原函数与 worker 片段：runtime 48 项、dispose 12 项 ASan/UBSan PASS。native API、时间与并发时点使用显式测试桩；不冒充硬件调度。
- `test_owner_matrix.py` 针对本目录实际源重新以 clang 编译 AArch64，执行两种 codec × 两个 slot：100 个 owner 正反例、24 个 early-close guard 例，共 124 PASS。
- owner 检查的所有 case 无非栈写入。early close 的 instance/arena/codec/WSTATE/active 标记错误均未进入任何 native 硬件 API；返回后 busy 和 close_lock 为0，close_failed 为1。
- 正常 close case 在首次 SUBMIT 处停下；此测试只证明允许合法实例进入原关闭序列，不证明 EOS、IRQ、FINISH、EXIT 的硬件行为。完整关闭由 sibling audio 的原厂流程证据与 root 整机 QEMU 覆盖。

输出：`owner-matrix.json`、`handoff-tests.json`、`source-sha256.json`。独立远端构建目录 `/path/to/user/fxfix/v52-portrait-review`；未接入其他正在运行的 GDB/QMP。
