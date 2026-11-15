# v52e 关闭后重开失败证据

两个诊断运行都加载 Eclose MAIN `3b976636e4b67c86a9e93b0d82c598d23a7ea3e73c4733b3a7b82b27f252e7cb`。这是 E 的编解码器加 C 的 worker-close runtime，用来强制验证关闭；它与生产 E 的 retained-owner 流程不同。两轮原断言均自然失败并停止，未修改模型、固件、寄存器或断言。

已观察到关闭完整清空实例、VDI、client4 映射和 worker，随后 hold 归零、IRQ 继续增加，新 W5 实例建立但 worker 停在 2、magic=0、native_instance=0、frames=0；encoded 固定 554。旧轮 IRQ 最终 2060，只读探针轮最终 2076。它不能记为“完整交接已通过”。

探针轮的第一轮 OPEN：

- `401 @ 40c38a74`：SET_PARAMS 原始结果 x8=0。
- `403 @ 40c3b6d4`：实际 VPU HEADER 命令结果 x0=0。
- `402 @ 40c33df0`：HEADERS service 结果 x0=0。

第二轮 OPEN：

- `401`：x8=0。
- `402`：x0=0xffffffff，即 service 返回 -1。
- 没有第二次 `403`；stderr 同样没有第二个硬件 HEADER ENC_PIC 命令。

插件记录无覆盖、寄存器读取错误为零。401 的 x8 特意保存在 record 的 x3 位置；`summary.json` 已明确标注。时间为 host monotonic，不能直接作为相机实际耗时。

这将首个可见失败限定在原厂 HEADERS service / worker 状态门，而不是 host codec 的 HEADER 返回失败。仅此观测仍未唯一证明旧事件 bit 的来源或是否为唯一路径。生产 E 的普通 REC 恢复通过，不能替代这个原厂关闭后的重新打开场景。后续候选必须保持完整关闭→重开→新编码断言。

原始 `result.json`、stderr、插件二进制记录和 driver 均在每个子目录；`summary.json` 保留关键返回值、RAM 状态及真实固件身份。没有真机验证。
