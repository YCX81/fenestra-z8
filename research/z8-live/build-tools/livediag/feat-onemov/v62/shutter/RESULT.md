# V62 快门声候选：实际音型覆盖与共同静音入口

## 已确定的问题

V61 `live_shutter_z8.c` 的 `allowed()` 仍是 V56 起的恒零实现，麦克风与原厂 SFX 的交接全部关闭。只把菜单显示成 On 不能恢复发声。

更早的交接实现只接受 tone 1/2。已复制的 V60 实机日志有 13 条 EV60：首轮原厂 prepare 后的选择是 **10/10**，随后仍为 10/10；driver=5、stock mute=0、native inhibit=0。V60 此时已经关闭交接，因此不能由此倒推 V55 的某次零计数的唯一原因；但可以证明简单恢复旧 `allowed()` 仍不覆盖当前用户选择的音型。

原厂 profile setting 是 `0x02006101`。SoundTask 将 prepare 参数的曝光有理数与 `34/6` 比较：较慢分支 profile 0..4 分别使用 `1/2、11/12、15/16、19/20、23/24`；另一普通分支 profile 1..4 使用单段 `10、14、18、22`。单段从 prepare 入口播放，之后的 fire 资格表不包含这些 tone，因此旧 `40f9cb64` 的 Live mute gate 没有覆盖它们。

## 候选与 ABI

`patch_shutter.py` 只接受精确 V61 两文件 SHA，并拒绝直接改交付的 `v61/work`。候选目录为 `candidate/`。修改只有 `src-fix37/audio/live_shutter_z8.c` 与 `hooks.S`。

1. 保留现有 prepare 汇编入口的寄存器保存、被动诊断和恢复，最后尾调用新的 C helper。
2. helper 只在 Live 请求有效、photo 模式、用户选择 Off 时复制原 24B 参数，清复制件 byte 8，调用原厂 `40f9cccc`。On、Live Off、视频、关闭或无 runtime 均传原参数；原参数本来禁音时也不强行开启。
3. 原厂 producer 只从 x0 读取 0/8/16 三个 Qword，立即复制到固定 `a03bc6ff0`。之后使用固定区与静态事件对象，不保存调用者指针。原调用点 `40f6cff0` 之后不读取返回值；helper 仍保留原厂 w0 返回值。byte8=0 使原 SoundTask 置 inhibit，prepare 和 fire 均不投递声音请求。
4. 恢复原有完整 Live/photo/mute/audio-owner 的 allowed 条件，扩大精确 finite tone 集合到 `1,2,10,11,12,14,15,16,18,19,20,22,23,24`。仍要求原 constructor、原 callback、repeat=1 和同一 native envelope；6/13/17/21/25 等循环协议不接管。
5. `normal()` 必须先验证消息 ID、param、tail、payload 和外层 callback，再读取内部参数和 tone。第一版曾在守卫之前读取 tone，被独立检查发现；现版本已修复并加入 unmapped 指针反例。
6. 将 `mic_wanted()` 的录音所有权条件与 `allowed()` 的发声许可分开。旧交接在麦克风已暂停后切 Off/原厂 mute，会跳过 resume 却结束 shutter 状态，留下 active=1、backend=IDLE 的录音假活状态；实际 ARM 复现见 `resume-mute-before-fix.json`。现在静音只取消 SFX，原生完成和所有权检查通过后恢复麦克风；Live Off、shutdown、原厂 codec hold、视频、inactive、cancelled 或 want=0 仍不恢复。frozen 必须等 mux thaw 后恢复。

新的 prepare 和 tone selector 放 primary text，避免扩大固定 shutter bank。最终候选 C SHA256 为 `5179121322cd6f37c5eac6b2d2d2ad5b1c2b115f22f2dac05bbd56709d7edb9f`，hooks SHA256 为 `c8e1e46f1644b07cfe43d461f52e4e4f87312536d68a2394c4cffca1f4efafbe`。本机 Clang 编译的该 C 分配为 `.text=328 B`、`.live_shutter=2280 B`，完整远端链接以主代理结果为准。

## 验证结果

- `test_adapter_arm.py`：**122 组通过**。实际编译候选执行，原厂构造、SFX dispatcher、SoundTask callback、释放协议执行；所有 14 个 finite tone 分别覆盖未转交取消与已接管后停止/完成/恢复。包括 9 类消息/指针守卫、65 个 tone 值精确筛选、7 类 prepare 参数/透传/返回值测试。新增 38 组在 before-pause、STOPPING、HELD stopped、NATIVE、DONE 阶段触发 Off/原厂 mute/超时，保持 want=1 检查恢复；并覆盖丢失所有权时禁止恢复、冻结后等待 thaw。结果 `adapter-arm.json`。
- `test_native_profiles.py`：**23 组通过**。直接执行交付 V61 原厂 producer、SoundTask、曝光有理数比较、构造及 driver5 拒绝回调/释放。参数 `6/30` 与 `6/42` 真实执行 `34/6` 阈值比较，复现 14 种 finite tone；五种 profile 在 prepare byte8=0 时 prepare/fire 都不提交。另外在同一原厂实例连续 On/Off/On/Off，tone10 只在 On 提交，inhibit 正确重置。原参数在 producer 返回后被毒化，固定区数据和后续 task 仍正确，验证同步复制而非保留指针。结果 `native-profiles.json`。
- adapter 使用早期 pinned 原厂映像的 native 协议代码；与 V61 的 SFX/SoundTask 连续区间逐字节比较，差异仅既有 manager 和 mute hook 指令。其它 native 字节相同，见 `evidence.json`。因此不把旧固件整图等同于 V61，而是限定复用范围。
- 对比完整 candidate 与 V61 复制源，只差上述两个源文件，无新增文件。补丁对交付 V61 原路径和源 SHA 不匹配均拒绝，并验证拒绝前后两文件字节不变。

复跑：

```sh
build-tools/livediag/feat-rec/.recorder-venv/bin/python build-tools/livediag/feat-onemov/v62/shutter/test_adapter_arm.py
build-tools/livediag/feat-rec/.recorder-venv/bin/python build-tools/livediag/feat-onemov/v62/shutter/test_native_profiles.py
```

## 独立 ARM 测试与整机验证边界

后续整机验证已完成 On 双段、On tone10 单段和 Off 三轮，均执行最终 MAIN `36c564c9…`；实际 PCM、原厂完成、录音恢复、保存和完整解码通过。完整结果见 `../verification/RESULT.md`，下文说明本 ARM 子任务本身的范围。

上述测试的 heap/OS/队列、mic stop/start 或下层 DAC 有显式桩，不等于扬声器发声。验证代理已确认真正 tone10 原厂 PCM 长度 28800B、150ms，需要私有 QEMU 支持该实际 DMA 长度；不能把旧 4800B tone1 fixture 冒充 tone10。整机需要原菜单选择 Type B、真实交接、实际 TX PCM 和环境录音恢复证据。

继续保留原厂 Silent 和用户静音，不强制音量、驱动状态或伪造完成。开启快门音时硬件录音/播放共用资源，原交接设计会在暂停麦克风期间补时间轴静音；声学延迟、切换噪声和真实调度仍需实机听测。循环/Bulb 协议没有在本次扩大接管。
