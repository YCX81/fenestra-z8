# V62 新拍 Live MOV 原生回放结果（已冻结）

**On-r2 和 Off-r4 的原生播放进入、返回原照片、临时标记清理均通过。完整源帧覆盖以 Off-r4 的 93 帧为准；On-r2 的末帧没有在原生输出中出现。** 未追加 QEMU 或扩大固件改动。

最终 MAIN：`36c564c9e785822ccf784816882d5055ee8e5ca0b796e1c4fb8e459dfc1ae4b5`。

RDW QEMU：`9804d5734610465613f80dc762dd3bef0307571b0b016f140cdec81252ba6123`，私有原生 SFX 模型版本，MOV 仍走原生 Wave5 decoder 路径。

| 新片来源 | MOV 源帧 | 原生 picture 事件 | 严格 CRC 对照覆盖 | 返回/清理 | native 检查 |
|---|---:|---:|---|---|---:|
| On-r2，Type B 慢快门 11/12 两段声 | 92 | 92 | 源帧 0–90 同序输出；首帧重复 1 次；末帧 91 未输出 | 通过 | 237/237 |
| Off-r4，UI 关闭快门声后新拍 | 93 | 94 | 全部源帧 0–92 同序输出；首帧额外重复 1 次 | 通过 | 235/235 |

不能将 picture 事件数直接当成已显示的源帧数。RDW 的 picture CRC 来自实际写出的 NV12 像素行；本次从各自已关闭的私有回放卡只读提取原 MOV，分别用 ffmpeg 解码为 NV12，再逐帧计算 CRC32 比较顺序。POC 在 GOP 边界会重置，因此完整结果同时保留事件序号、POC 和 CRC。

Off-r4 的事件 0 是重复首帧，事件 1–93 与源帧 0–92 逐项完全相符。On-r2 的事件 0 是重复首帧，事件 1–91 与源帧 0–90 相符，源帧 91 的 CRC `c923fa0a` 未输出。这里只记录已观察到的尾帧限制，不将其解释为新固件回归或声称已经定位原因。

两轮都从实际新 MOV 进入原生 movie 页 `(3,0x56C)`，返回 photo 页 `(3,0x552)`；bridge start=1、return=1，stage 2→3→0，临时 nb_bit 1→0，nb_fail=0，最后选中原照片 DCF ID `0x03200020`。观察点的 nb_cleanups 仍为 0，清理结论使用 stage/bit/返回原照片，不声称看到计数自增。

## 新片与卡的身份

两张原始捕获卡都位于：

`/path/to/user/.local/share/nikon-z8-qemu/live-audio/regress/runs/`

- On-r2：`v62-36c5-tone10-on-r2/capture-card.raw`；新 MOV SHA `21142258ce8c114d1391f72452657a645990a8a31239665041a5813d04977b19`，1,088,316 字节。
- Off-r4：`v62-36c5-tone10-off-r4/capture-card.raw`；新 MOV SHA `9415a5749e2e302e44b75d2376846bb4432246df4051ed2975ea3235efade21c`，1,101,375 字节。

捕获 guest 完整退出并导出完成后，独立 fuser 确认卡未被打开，再制作私有副本供回放。原始卡未修改，回放后大小与 mtime 保持不变。回放私有卡提取出的 MOV SHA 与捕获导出一致，目标都是各自卡上的 `LIVE/100NCZ_8/DSC_0001.MOV`。

## 范围与记录

这是**同一保存卡副本重新启动后的回放**，不是同一次开机拍摄后立即回放，不是双卡或相机实测。未用旧 JPEG preview 证明原生 MOV。播放器音频输出为 none，音轨完整性和快门 PCM 由 capture 任务另行验证。

On-r3 的实际 tone10 捕获及 ffmpeg 验证已由 capture 任务完成；这里只保留私有快照和收据，没有进行第三次重复原生回放。

- 汇总：`verification.json`
- On-r2：`on-r2-reboot/summary.json`、`frame-coverage.json`、`decoder-picture-events.log`、`replayed.MOV.extraction.json`
- Off-r4：`off-r4-reboot/summary.json`、`frame-coverage.json`、`decoder-picture-events.log`、`replayed.MOV.extraction.json`

On-r2 的原 16 项通路检查保持通过；加入严格源帧覆盖后，summary 总体 passed 为 false，并单列 `native_decode_return_cleanup_passed=true`。Off-r4 的通路和完整帧覆盖均为 true。汇总按这两个不同结论签收，不隐藏尾帧限制。
