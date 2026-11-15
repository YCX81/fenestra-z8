# HEADERS 输出寿命修复

候选只修改 `work/src-fix37/codec/live_w5enc.c`：把 hdr、byte count 和 4 个 NAL length 从 `open_stream` 的调用栈移到 `w->scratch+0x720` 的 32 字节结构。原厂实际写 28 字节，64 位首字段 8 字节对齐；原有 RUN 区 `0x700..0x710`、SUBMIT 区 `0x800..0x840` 不重叠。加入编译时布局/范围断言；`live_w5` 大小仍 1057024，scratch 偏移仍 784，没有 runtime ABI 变化。

## 测试

`test_lifetime.py` 编译 E 基线和候选各自的真实 `open_stream`，把 ARM64 对象及常量加载入 Unicorn。配置/注册等前序步骤作为成功 stub；HEADERS 执行原厂 `0x40c38af8` 服务，event wait 模拟超时。两份实际 caller 都返回 -9；原厂已经保存了 3 个输出指针。

随后将旧调用栈全部填入新用途的 canary，再执行原厂迟到完成 `0x40c3ccfc..0x40c3cd88` 的实际指令写入地址、长度和 4 个 NAL 长度：

| 检查 | E 基线 | 候选 |
| --- | --- | --- |
| 输出指针 | caller stack | retained pool scratch |
| 已重用调用栈被改写 | 28 字节 | 0 字节 |
| 原厂迟到 28 字节回复 | 写入成功 | 写入成功 |
| 相邻 RUN/SUBMIT canary | — | 全部保留 |
| open_stream 对象大小（clang O2） | 1088 B | 1108 B |

输出 `test-result.json`，完整对象也通过 AArch64 freestanding 编译。这是超时后的软件寿命反例，不说明 Eclose 实跑有发生 HEADERS 超时；Eclose 的另一个问题已由 passive observer 定位为初始化状态尚未就绪时 HEADERS 被拒绝，由独立候选解决。

## 为什么该内存仍然有效

目前错误路径会使 W5 magic 为 0，但 `live_stream.release()` 不清 `encoder.hw`，也不释放 W5 自己的两个 pool；`live_encoder_dispose()` 在 magic 不匹配时拒绝释放。后续 `live_encoder_init()` 看到原有无效 hw 直接返回。因此候选迟到回复所在的 W5 pool 保持分配，不会变成已释放内存，也不会被下一次 `open_stream` 清零复用。此事实同时意味着现有错误状态会永久 pin 住资源；本补丁不把它描述为重复分配/覆盖 hw，也不尝试用盲目释放或重试解决。

## partial-open 后续设计（未实现）

必须区分“持有 native 资源”和“已经可以编码”，不能只提前设置 magic。可利用经全源确认未被使用的 reserved 字段保存 open phase/error，保持 ABI，但需要单独审查所有读写点。fresh hw 的发布顺序也必须先设置 busy，再发布有效 magic/hw；open 返回前保持 busy，不能让原厂 preempt 关闭半初始化实例。

- 从未发 SET_PARAMS、确认 worker 0 / 无活动 instance / 独占 client 和 arena 时，可研究正常 EXIT 并核验 VDI、slot、map、instance 后释放。
- SET_PARAMS/HEADERS 请求已交给 native 后，要保留请求输出存储和 pool。state 1/3 表示异步工作，不可重发配置、清空 reply 或释放。继续等待必须有阶段及截止条件。
- state 2 没有已证实的安全 abort API。`40c37bac` 虽对 state 1..5 调用 VPU close，但忽略返回且不将 native worker state 归零；随后 service deinit 要求两 worker 均为 0。直接 EXIT 还会忽略 service close 错误并清 map、复位/掉电，不适合作为 partial rollback。
- 到达合法 state 4/5 后才可用已验证的正常 drain/finish 路径；instance 绑定、codec、arena、client/slot、无其他 owner 的检查仍须保留。若不能证明终止，保留故障和资源并拒绝 handoff ACK。

F 候选只包含前面的输出寿命修复，以上生命周期设计没有合入生产。
