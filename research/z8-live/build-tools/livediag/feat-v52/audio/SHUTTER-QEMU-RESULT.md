# 快门被动诊断的整机 QEMU 取回验证

**已验证诊断可落入虚拟卡并取回；未验证真机快门声恢复。** 没有启动新 QEMU，没有修改固件源，也没有操作相机或物理卡。

生产 v52c（MAIN `4790c3e6138f4f22a5c189988e3ba7338de8f315b9bb3515930b3b97132ac00f`）的 `v52c-rec-recovery` 导出 `LIVEDATA/LIVELOG.BIN` 为3872字节，LLG2头有效，保存时刻112430590µs。其中包含全部四条 EV60：

| 记录点 | 时间 µs | incoming | prepare/fire state | prepared | capture phase |
|---|---:|---:|---|---:|---:|
| full_press | 88241122 | 0 | 60 / 60 | 0 | 2 |
| prepare_message | 88244167 | 1 | 60 / 60 | 0 | 3 |
| fire_message | 88444113 | 0 | 1 / 2 | 1 | 3 |
| mute_gate | 88445595 | 0 | 1 / 2 | 1 | 3 |

四点共同状态：`driver=5`、`audio_active=1`、`service=0`，`sound_running=0`、`stock_mute=0`、`inhibit=0`。这证明该次 QEMU 的录音软件状态仍活动时，prepare/fire/mute hook 均到达，并由正常 worker 日志写出；没有证明 DAC 输出或硬件声学结果。

另外两轮也独立取回完整四点，状态变化一致：

- `v52c-close-diagnostic`：同一生产 MAIN；四点时间89200107、89202176、89399114、89401636µs。
- `v52c-audio-protocol3`：私有 `PB_AUDIO_TEST` MAIN542c2553…，仅用于仿真；四点时间88010107、88010234、88180113、88183013µs。该宏用于之后的 REC 音频服务，不应把它称为交付镜像。

`v52d-hevc-close` 导出的日志没有 EV60：其保存时刻只有20000142µs，已有记录仅覆盖到17795110µs，事件类型7/23/25；现有 checkpoints 结束于 Live 菜单导航，尚无 full-press contact。这份文件不覆盖快门测试，不能据此判定某 hook 没执行。四个导出目录都没有 `LIVEPREV.BIN`。检查时 E/Eclose 仍在运行，尚无导出的这两种卡日志，本说明没有把 C 的结果写成 E 已验收。

原始 BIN、每轮解析 JSON、运行 manifest（含 firmware/QEMU/driver hash）和覆盖说明均在 `shutter-card-evidence/`，汇总 `shutter-card-evidence/report.json`。用现有 `read_shutter_log.py` 读取，未修改解析器或固件。

可用于交付说明的措辞：**快门声问题仍待真机定位。本版附带被动诊断，已在整机 QEMU 验证四个记录点能写入并从 `LIVEDATA/LIVELOG.BIN` 取回；QEMU 没有真实麦克风 PCM 和可据此确认的扬声器输出，因此这不是快门声修复验收。**
