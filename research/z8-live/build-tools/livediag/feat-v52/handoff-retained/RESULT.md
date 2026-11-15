# v52 retained owner / HEVC close candidate

状态：三文件候选与离线检查完成，完整 QEMU 由 root 以最终合成镜像运行；未操作相机、USB 或实体卡。

## 已观测故障

C 的实际加载 MAIN 为 `4790c3e6138f4f22a5c189988e3ba7338de8f315b9bb3515930b3b97132ac00f`。`v52c-close-diagnostic` 使用同一固件，新增只读稀疏观察，REC 完成 45 帧后恢复失败。进入照片回放前 `closes=0, close_failed=0`，进入后 `closes=1, eos_miss=1, fin_rc=0x01000000, close_failed=1`，worker=4、slot_used=1，实例头 `(1,0,0,1)`。最终 IRQ 继续到 2068，而 encoded 停在 545。stream/audio 存储已清空，仍保留 Live 的 W5 实例。原始报告路径、SHA 与三个检查点保存在 `v52c-close-failure.json`。

源代码的 stage 1 拒绝条件将 `inst[3]` 固定为 3。这是 AVC 类型，并非运行状态。原厂 worker `40c3b2ec..40c3b308` 将 Nikon codec 0/1 转为 open parameter 0/12；`40c58060..40c5808c` 将它们分别写为 instance+0xc 的 3/1。因此 HEVC 的关闭被错误拒绝。此缺陷也存在于 v51 的关闭代码，足以解释交接不能完成的一个确定原因；本次 QEMU 观察不能单独证明用户每一次真机沙漏的唯一原因。

## 改动

基线是只读 `feat-v52/integration-c/work`，严格前后 SHA 在 `changed-files.json`；交付为 `retained-handoff.patch` 和 `work/` 中对应三文件。

- `live_w5enc.c`：关闭与保留所有权检查严格匹配构建编码类型（AVC 3 / HEVC 1）；关闭还验证实例指针、arena 与 RUN 时绑定相符。保留 `close_failed`、忙状态、请求状态和所有原关闭时限。
- 普通照片 REC 只停止 stream/audio，保留原 W5 实例。退出后须先确认没有 worker 存储或 mux job，再在 handoff 锁下检查 VDI 为零，或仍是 Live 独占的原实例。检查 client 4、其他 client 全 ff、双 slot 占用、arena、worker、instance 指针和 codec；最后 CAS hold1→0。
- 原厂视频/录像仍走 hold2→preempt→worker/audio ACK→second preempt→dispose。没有解除失败关闭的保护，也没有改变超时后仍转交原厂的既有限制。
- `native_instance` 用尾部既有 padding：真实 ARM `offsetof=1057016`、`sizeof(live_w5)=1057024`，均有静态断言。client 表初始化地址 `402f2a98` 明确写前 4 字节 ffffffff、第 5 字节 ff；有效 client 仅 0..4，检查不包含 padding。

普通 hold1 恢复查询与 hook 共用 `stock_handoff_lock`，避免 hook 回收 W5 pool 时查询悬空指针。hold1 路径不会关闭 REC 扬声器。hold2 的 PB quiet 查询保持。

## 离线验证

- 实际 runtime 状态机 / worker 48 项、实际 disposer 12 项，ASan/UBSan 通过。
- 实际编译 ARM retained helper：AVC 和 HEVC 各 25 项，通过；所有非栈内存写入数为 0。
- 实际编译 ARM close/release：两种 codec 共 8 项，通过。匹配类型执行 STOP→TRIGGER→TRIGGER→FINISH→EXIT；错 codec、arena 或 instance 绑定在 stage 1 拒绝。原厂返回和完成状态由 harness 提供，因此不代替整机关闭验证。
- ARM 编译 `-Wall -Wextra -Werror` 通过。相对 C 两个 object 合计主段约 +588 B，extra -164 B；完整链接与真实 cave 边界由 root 检查。

建议 root 的单变量诊断镜像：新 `live_w5enc.c` / header 加 C 原 runtime，真正走 worker close 路径；它仅用于验证 EOS/EXIT，不能作为最终交付版本。生产候选使用本目录三个文件。
