# REC 结束后全局 reset 模型冲突

本项只读分析，不修改固件、模型或 MMIO 容忍范围。旧整轮结果仍为 235/236，不改成 PASS。

实际失败镜像为 F `b0cd7c40…`，QEMU 为 `9919f344…`。REC 已在 guest 时间 29.749030 秒停止，最后运行快照的模型虚拟时间为 38.203163744 秒；没有记录未知访问自身的时间戳，不能给出更精确时刻。

实际访问：CPU0、PC `08201164`、LR `40d6a70c`，写 `0d154340 = 01ffffff`（4 字节）。它是 AArch64 低地址 service 代码，**不是 MCU 指令**：

- 原厂 `40d6a6c0` 遍历 0 至 312 调用 `40c0a0c4`，然后 `DAIFSet #3`，调用 `40c01720`。
- `40c01720` 跳到 `0820102c`；复制的 MAIN service 在 `40195164` 对应上述 PC，实际指令正是向 `0d154340` 写 `01ffffff`。
- 所列函数区间 G/F 字节完全相同；见 `native-identity.json` 和 `native-disassembly.txt`。

实际 v2 模型已有精确 `service_controller_write` 适配器，允许这个地址、值和宽度，运行命令也打开了它及 bootstrap fixture。但是该调用位于更后的分派位置：

1. 第 21667 行先调用 `video_reset15_access`。
2. 此函数检查地址及 bit15；`01ffffff` 含 bit15，因此被选中。
3. 它只允许值等于 `8000`，遂产生 unknown 并截断后续分派。
4. 第 21935 行的 `service_controller_write` 无法到达。

这是已定位的模型请求分派冲突，源文件/二进制身份及完整函数见 `model-evidence.json`。仍未直接采到进入全局 service 的上层事件，也未验证全局 reset 对所有已实现设备的状态影响，因此没有简单前移 handler 或吞掉该访问。不能把现有 service 适配器的记录行为当作真实硬件 reset 语义。

下一整轮沿用户实际操作测试 REC 后半按返回拍摄、重新达到真实 Ready；这可验证所需交互，但不代表长期静置或系统复位路径已验收。原静置失败作为独立边界保留。
