# G 初始化完成条件独立审查

结论：在保留当前请求前 state 0、原 `SET_PARAMS` 返回成功、实际 state 2、deadline、client/slot/arena/codec/instance 绑定的前提下，支持移除 `0xa02d32788==0`。该返回槽不是持久完成标志，要求它始终为零会拒绝成功实例。此审查支持继续整机验证，不替代 G 整机回归。

## 原厂 state 2 的发布

已检查 native worker `0x40c3a47c..0x40c3dd5c` 的 state 写入及其 global state 表引用。encoder state 2 的唯一写入是 `0x40c3d8ac..b4`，前面必须依次通过：

1. `0x40c350b0` GetInitialInfo 成功（`0x40c3c4c8` 调用，非零进入失败）。
2. 原厂帧区容量检查（`0x40c3c9d4..d8`）。
3. `0x40c350e8` RegisterFrameBuffer 成功（`0x40c3ce74..7c`）。
4. `0x40c35150` linear frame allocation 成功（`0x40c3d888..94`）。

上述失败分支只写 result=-1 并发 event 0x20，不发布 state 2。创建实例失败、InitSequence 命令失败也不会发布 state 2。其余 state 表直接引用为读操作、worker 启动清零，或其他状态的转换；没有发现失败路径把 encoder 设置为 2。

## 实际 ARM 反例

`test_native_w5_ready_failures.py` 对真实原厂机器码执行 9 种分支，不依赖旧的 90-fixture 对结果槽初值的假设：

| 注入点/条件 | 最后 state | 2788 |
| --- | --- | --- |
| VPUEncOpen 失败 | 0 | -1 |
| InitSequence 失败 | 0 | -1 |
| GetInitialInfo 失败 | 1 | -1 |
| 帧区容量不足 | 1 | -1 |
| RegisterFrameBuffer 失败 | 1 | -1 |
| linear frame allocation 失败 | 1 | -1 |
| 初始化成功尾部 | 2 | 0 |
| 已在 state 2 时重复 INIT | 2 | -1 |
| 已在 state 2 时再次 sequence interrupt | 2 | -1 |

接着保持 state 2/result -1，执行真实 HEADER worker `0x40c3b668..0x40c3b710`，仍然调用 command 0x33，并在命令成功后进入 state 3。测试结果见 `native-w5-ready-failures.json`。

失败 API 的返回值和 RTOS event post 为测试边界，分支和 state/result 写入均为原厂指令。本测试没有确定整机究竟是哪一种后续事件覆盖了 2788，也没有模拟完整硬件/RTOS 时序。

## 不放宽的条件

只读 state 2 本身不足以确认这是本轮 Live 的实例。G 仍需拒绝非零初始 state、原 SET_PARAMS 错误返回、其他 client/slot 占用、错误 VDI/arena、instance 更换或失效、codec 不符，以及超过截止时间的 state 0/1。对本次真正失败分支，state 2 等待不会误判成功：它们仍为 0/1，最终超时或由其他 guard 拒绝。

这项修正不清全局状态、不消费 event、不重发 SET_PARAMS，也不重跑 BRINGUP。HEADERS 输出寿命修复是另一个独立补丁。
