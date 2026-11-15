# 结合版独立音频审查

当前验证 portrait 生产候选补丁 **de9f3c8532bcdfa5dc39b0388379c99f2699c7b9d9ac20dace52b8e4aeaa69b7**，测试前逐一核对其 9 文件 manifest；没有修改 portrait 源。

旧 691fb636 的四组 PCM 窗口 PASS 不是完整 adapter 通过。它随后被 playback 找到 partial resume + 新 held + cancelled 的状态机死锁，已经否决。当前用联合测试补上该路径，不能沿用旧结论。

- `partial_resume.py` 编译实际 adapter、backend、core。只替换 native 函数/MMIO 边界；在 resume 第二个设置消息拒绝时注入一份有效的新 held 请求（明确的并发发布边界）。实际逻辑将它 reject→DONE 后必须调用 backend 原 stop、投 108；未 ACK 前和 ACK 后仍有 entered callback 时均保留 descriptor，直到 drain 后回到 normal cleanup 才释放。ASan/UBSan PASS。
- `old-cleanup-counterexample.json`：仅在测试副本撤回 STARTING 清理条件，准确重现“没有 108、driver4 一直等 idle”的断言失败，验证这不是无效的通过测试。
- `run.py` 五组实际 C/原文 readiness 测试 PASS：一个三秒窗口内两次暂停准确形成 1s PCM/200ms gap/250ms PCM/100ms gap/1450ms PCM；mark 与绝对窗不丢；cold held 不提升 Ready；resume/cancel 重新判断；bridge 不能补缺少的起始历史；freeze-after-resume-before-first-PCM 不 bridge、不 stop 正在被 mux 读取的 frozen ring，thaw 后建立新历史；frozen reader 拒绝 resume。

这闭合了已发现的 C 状态机反例，不等于完整原厂调度/真实音频验收。原厂 SFX 构造/回调/heap 的实际 ARM 用例由 portrait 覆盖；低层最后 PCM111 与 stopdone116 producer 顺序现已有 portrait/capture-stop-order.json 的实际 G ARM 两组证据：705 先设置 stopflag，event worker 先投 pending full/partial PCM，再投 stopdone；high callbacks 为 PCM9600/PCM400/108ACK，后续 ADC producer 见 stopflag 不再投事件。硬件 serial drain 与 OS 队列边界仍为 shim；不把软件 producer 顺序当作已经模拟了真实 IRQ 临界区。当前无麦克风的 QEMU 应停 Preparing，不通过 RAM 改变量假造 Audio On Ready。
