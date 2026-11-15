# v52b REC 返回后 Live 不恢复：已定位源码自锁并给出增补

失败证据：`v52b-failure.json`保存原result路径和SHA、固件SHA与关键checkpoint；原远端完整run/result/log保留。实际 `-kernel` 加载固件 `/path/to/user/fxfix/v52-root/v52b/q/main.main`，SHA `ed053fac18e3f339cf5d9db8bc0747277ad4518a5cabd15b812da109f4c2a289`。result顶层 `firmware_sha256=2b43a31c…` 是 driver 的 prepared-stock baseline，不能视作执行镜像；此处已纠正最初误引用。`live_a.executed_main_sha256` 与实际文件独立hash相符，另有QEMU启动loader资源输入，未声称纯裸MAIN硬件仿真。

REC完成45帧、PB error0。半按返回后原厂取景IRQ由1546增至2040，Live编码始终546；capture.descriptor/fr_desc/audio.active/audio.descriptor均0、音频IDLE；hold始终1、handoff_lock0；VDI仍16010000，W5 worker/client4仍存、encoder.hw未清。此次不是“原厂取景IRQ没有恢复”。

源码闭环：新early照片回放hold1释放stream与音频，但`live_stream release()`不关闭W5。REC预览不用W5，因而没有原厂bringup hook替Live关闭它；`live_stock_hold()`见自己的VDI仍非零，持续刷新恢复epoch，无法解除hold。这是v52新增earlyhold路径的具体遗漏，不能归为QEMU偶发或旧A2模型问题。

修复：所有worker buffer和采集音频quiet后，显式handoff仍CAS2→3交给原厂caller。普通hold1且encoder存在则尝试取得同一个stock_handoff_lock，重新确认hold1，调用既有`live_encoder_release`关闭自己的W5，再由原有严格dispose检查释放pool。锁防止原厂hook同时读到已free的hw；close失败保留pool和hold，不伪造quiet。PB speaker handoff仅hold2才调用，普通hold1不会停用户正在看的REC音频。

`handoff-recovery.patch`基于此前handoff READY，仅改`src-fix37/runtime.c`。包含root后来提出的`live_ui_pb_audio_handoff`检查，请三方合并，不覆盖root整文件。`live_w5_bringup_notify`和`live_stock_release_ack`已标`.live_extra`,noinline；实际ARM对象这两函数共1360B，父Agent负责放到401fd218..401fe000并提取/拼接，避免Live主段空间不足。

测试：`test_handoff.py`运行真实源码函数/worker片段，46项runtime/worker检查＋12项安全dispose检查通过，ASan/UBSan。新增覆盖earlyhold关闭自身VDI后新IRQ恢复、锁忙不碰pool、音频未停不close、close失败保留、PB speaker busy→quiet才ACK、hold1不调用PB speaker handoff。`build-runtime.json`确认ARM `-Wall -Wextra -Werror`编译通过。最终综合候选须保持原REC/IRQ/encoded断言重跑，当前没有用单元测试冒充整机或硬件通过。
