# 最终外部 driver 准备（未计整机通过）

执行镜像固定 F `b0cd7c40…`，actual AArch64 headers 生成 ABI23744。driver 仅输入原始按键/GPIO、明确 PCM 设备输入和声明的启动 ROM fixture；未修改 guest RAM、寄存器、Ready 或生产固件 timeout。

与 Ready3 的差异：

- `pcm_feeder.py` 用真实 serial RX 请求 bit 区分停机与暂时无 armed bank。IRQ/rearm 期间保留绝对50ms deadline，只有 RX bit 关闭才结束 epoch。200次rearm主机测试未累计漂移。
- PCM capture 尚未成功提交时，错过原先的迟到门槛仍明确失败。已有一张成功保存且job0之后的迟到只能记为外部 stimulus gap：丢弃过期输入、下一个当前时刻+50ms再供新块，不补发旧块。不能把存在gap的整轮宣称连续录音通过。
- FULL及从相册返回后的 `pb-recovered` 都最多等60s virtual，要求实际audio_warm0/active1/capture.phase2。仅encoded递增不足以通过。
- 新增shutter.began/tone/cancel_posted/wake_cancel、audio anchor/frames/frozen等只读观测，区分自然完成与取消完成。原始completed计数不独自证明正常发声。
- `kernel_identity` 在launch前实际读取kernel并校验SHA；prepared-stock旧标签保留且明确分开，结果需结合实际command和manifest审计。
- 可显式传 `--sfx-resource`，严格核对portrait候选60928B/SHA `eabd3f39…`，仅启动加载ROM46460000。它将官方MAIN的tone1 PCM用作synthetic tone2；不是恢复的真机tone2音色/时长。

下一整轮等待严格TX模型、资源fixture及独立review。模型所有tuple、长度、phase、内存范围与stop guard不变；物理扬声器、麦克风与音色仍是未验证边界。
