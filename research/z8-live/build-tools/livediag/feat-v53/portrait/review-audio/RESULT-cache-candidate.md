# v53 扬声器输出补丁独立复核

结论：未找到需要阻止候选回归的新缺陷。只读审查与隔离测试完成；不能据此称真机扬声器、耳机/HDMI 或实际声画同步已通过。

审查身份：`audio/work/pb-fix37/runtime.c` SHA-256 `412675da6029c4ca56ce655e55c847be72d64b512d420d7f672a82c4922f1844`；只包含 `speaker-output.patch` 的输出阶段修改。实际原厂指令使用已交付 v52 G MAIN `ed1e8496fccde443412e527a252de5029a55c749053b9b046da42bfc454aa613`。本目录测试未修改该源码、旧镜像或运行中的 QEMU。

## 原厂与候选指令证据

- 在 G 镜像执行 `4066967c -> 403f7e64 -> 40e35828 -> 43180017`，与实际候选 `audio_output` 编译 AArch64 的 28 字节输出比较：音量 0..36 × 两路连接状态各 0/1/2/5，共 **592 组完全相同**。音量 u16 `a033868d0` 未写回，0 仍为 0；没有固定最小音量或强制开扬声器。连接状态查询 `403df4e4(4/6)` 是原厂 RAM getter。原厂 UI 用已更新的第 6 路缓存，候选直接查当前第 6 路；比较时缓存与真实连接一致，此有意差异不伪称所有旧缓存情境字节一致。
- G 原厂 service `40d24ae4` + driver `40c81ec0` 的三组对照重跑通过：缓存有效且 clean，OPEN 后路由全零、START 仍能到 driver3；显式 OUTPUT17 后出现原厂 `42040908`/`42040906` 并设置路由/音量；dirty 缓存路径能自动达到同样结果。音频队列/DAC 仍为观察桩，不能证明物理发声。
- dispatcher `40d24b84..88` 的 OUTPUT17 失败标志是 payload+28；`40d24bb0..c0` 才回调请求事件号。新 stage8 检查 +28，与旧 OPEN/START 的 +20 和 BUFFER 的 +21 分开；+20 同时是外接输出 enable，不能拿它当 OUTPUT 的成功位。候选正确。

## 状态与反例测试

用实际 `audio_callback`、`audio_begin`、`audio_step`、handoff gate 源码，只有设备地址/I/O/队列/时钟和 ARM cache 指令做 host 适配，ASan/UBSan 全过。除现有完整 PCM、边界、忙等待、取消、STOP 重试测试外，独立追加 **15 个**失败时序：

- 9 组音量零/路由组合，`1b` 流块通知不当成 ACK；外接 enable=1 但 +28=0 不能启动 BUFFER，转 STOP。
- 2 组 stage8 取消先于 positive/negative ACK；到 700000µs 边界仍保留 payload，迟到 ACK 只能转 STOP，STOP 确认前不放 PCM 所有权。
- 4 组 stage8 超过 700000µs 后迟到 ACK、再次进入 REC，含有/无 handoff：保持 stage6、保留音频所有权和 payload，不重发、不复活 START、不释放缓冲区。
- 新 stage8 已加入视频首帧等待条件；hide/handoff 会设 audio_stop。新阶段没有另开重入或覆写未完成消息的路径。

超时后 stage6 永久保留是当前的既有安全边界：不会自行恢复音频资源。测试只证明它不提前释放/重用，不证明遇真实无回复后用户可以无重启恢复。

## 范围

本审查未引入候选源码修正。保存 `lifecycle.json`、`profile-arm.json`、`native-output.json` 与三个可重跑测试脚本。未发现原厂 producer 会向这一候选请求重复发送终结 ACK 的证据，因此未把任意伪造事件注入产生的行为当成实际缺陷。物理路由切换、实际播放响度、用户播放过程中调音量和真机最终行为，仍以候选整机/硬件回归为准。
