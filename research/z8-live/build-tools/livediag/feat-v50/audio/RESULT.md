# v50 录音增益与快门声

## 已修复：录音每 8192 帧切换增益

基线是从 win-lab 的 `/path/to/user/v37build/v49/work/src-fix37/` 读取的实际 v49 源码。副本在 `baseline/`，修正副本在 `patched/`；不修改已有 v49 交付。

`runtime.c` 的 `live_audio_ticket_get()` 把 `live_audio_get()` 返回的 **双声道帧数** `*got` 当成 `int16_t` 样本数，对 `out[0..got-1]` 做饱和 4 倍增益。实际输出是 `out[0..2*got-1]`。旧 writer 每次取 64 KiB，即 16384 帧，因此每块前 8192 帧增益 4 倍、后 8192 帧增益 1 倍；8192 / 48000 = 0.1706667 秒。这与 `feat-w5dec/README.md` 记载的真机 PCM 周期相符。

`fix_audio_gain.py SOURCE_DIR` 只把该循环的上界改成 `*got * LIVE_AUDIO_CHANNELS`，保留原有增益和饱和，不增加实时 codec 写入，不改变录音或快门任务时序。脚本要求旧代码精确匹配，已经修正时可重复执行。

验证：

- `test_audio_gain.py SOURCE_DIR` 从真实 `runtime.c` 提取完整回调，链接真实 `audio/live_audio.c`；ASan、UBSan 开启。
- v49 对照在 64 KiB 块第 8192 帧失败：右声道实际 100、预期 400。见 `baseline-test.json`。
- 修正后通过：双声道、8192 帧接缝、1/3/8191/16384/24024 帧分块结果一致、正负饱和、非整块尾部、输出边界、零长度与取消读取。见 `patched-test.json`。
- win-lab 上重复同一实际源码测试通过；标准构建脚本的 AArch64 `--compile-only` 成功编译全部 20 个编译单元。
- 最终集成源 `/path/to/user/v37build/v50b/work/src-fix37/` 再次通过相同 ASan/UBSan 测试。最终 MAIN `4cb5235fc709220f64af5400680653999d2a03b0d5a4821975e0f6a7b2d50977` 的 `live_audio_ticket_get` 位于 `0x401e512c`，函数 120 字节与该构建的 linked payload 完全一致；反汇编确认 `got` 后有 `lsl #1`。见 `integrated-host-test.json`、`integrated-gain-bytes.json` 和 `integrated-gain-disassembly.txt`。

## 最终 v50 的并行 QEMU 回归

针对上述 `4cb5235f...` 最终 MAIN，使用隔离目录、独立虚拟卡和 named-pipe 通信，并行运行两个真实菜单操作用例。回归驱动没有直接修改 guest RAM 或强制 Live 就绪；只增加被动执行点与状态读取。完整统计在 `qemu/qemu-summary.json`，卡上导出的文件、最后检查点、原始被动追踪和截图在 `qemu/runs/`。

| 用例 | 结果 |
| --- | --- |
| `v50-sound-on` | 编码 925 帧，Live 事务 1 次、错误 0，JPG/MOV 写出；原厂 sound id 2 启动 1 次 |
| `v50-sound-off` | 首轮菜单退出后取景停住，编码 0、未产生拍摄、没有声任务事件；不算通过 |
| `v50-sound-off-retry` | 一次重跑成功：编码 933 帧，Live 事务 1 次、错误 0，JPG/MOV 写出；gate 到达 1 次，原厂 sound id 2 未启动 |

成功的 On/Off 两轮都有原厂 sound id 1 启动一次，区别只在 id 2。Off 的保存配置是第 2 代、`mute=1`，CRC 正确；On 是第 1 代、`mute=0`。这验证现有快门声菜单开关在仿真中的分流，以及合入增益修复后仍可拍摄保存 Live。

两轮 `audio_tracks=0`、`audio_skipped=1`：QEMU 没有为这两段生成实际录音轨。它们不能证明真实麦克风/扬声器并发，也不能代替音频增益的实际源代码和 PCM 单元测试。此次没有把“声音 API 被调用”写成“真机已能听见快门声”。

## 快门声：未修改不确定的原厂条件

历史现场记录（2026-11-13，v46）支持：Live 麦克风录音开启时快门声路径没有到达 `live_shutter_gate`；保持 Live 开启、仅临时关闭录音后，快门声恢复，`shutter_seen` 从 1 到 2，Live 保存成功。声状态字 `0xa0292be1c` 在 Live 开、关的空闲读数都是 12，不能据此断言按快门瞬间的状态。

本次重新反汇编基线 `c770f34014849b2c42f20b9258a32b8959253cfb296e48f83417572781e00864`，结果保存于 `shutter-disassembly.txt`：

- 声任务入口 `0x40f9c5cc` 根据事件 `w0/w19` 的 bit 0/1/2 分流，bit 2 才触发快门声后段。
- `0x40f9c698` 调用 `0x40ecb2c0`（跳至 `0x40f230a8`）；继续追踪确认这是两个 `{分母,分子}` 数值的比较器：先用 `0x40c78518` 归一分母，再比较分子。调用者构造 `{6,34}`，比较结果选择不同的声音状态表。它不是已证实的录音资源判断，不能把“返回不为 2”直接解释成录音阻止声音。
- `0x40f9cb34..0x40f9cb50` 再判定状态字是否允许；之后才会进入现有 `0x40f9cb64` 静音钩子。

这是待观测的分支，不是已证实的根因。不能用“强制允许”代替对声音服务并发能力的确认；也没有证据证明暂停/重启麦克风能保留完整预录。此次不改变这些条件。下一次真机诊断应在快门事件时捕获事件位、`0x40f9c69c` 的返回值与 `0x40f9cb34` 的状态，而不只在空闲读状态字。

没有进行 USB、相机操作、上卡或刷机。录音输出是否消除用户听到的所有杂音、快门声的具体硬件互斥条件，仍需真机验证。
