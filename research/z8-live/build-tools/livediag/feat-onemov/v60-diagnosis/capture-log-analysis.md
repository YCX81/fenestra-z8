# V60 新卡拍摄日志分析

结论：日志支持 9085 和 9089 的 Live 文件事务成功；9086–9088 在 9085 保存期间拍摄，没有接受新的 Live 工作。这能解释这三张没有 Live 文件，不能解释完整产物 9085/9089/9090 仍无回放标志。

## 日志与照片关联

`LIVELOG.BIN` 是 LLG2，stream head=92、crumb head=43，128 条 stream ring 未覆盖。32 条 crumbs 与 stream ring 有重复，按完整四元组去重后为 92 条事件。`LIVEPREV.BIN` 只有 10 条事件，没有拍摄事务。不能把重复 crumb 当成重复快门或事务。

五条原厂 WRITER 与 9085–9089 的 JPG 文件时间按顺序匹配，推导统一开机时刻约为 **2026-11-15 14:58:22.083 +08:00**，最大残差 7.1 ms；两张 MAN 的时间与 MUX_END 也相符。日志没有直接记录照片编号，关联来自这些时间证据。文件 mtime 不被当成落盘完成的独立证明。

| 照片 | 日志事件（开机后秒） | 结果 |
|---|---|---|
| 9085 | PRESS 25.598 → FREEZE 27.139（93 帧）→ MUX 28.631–34.941 | rc=0，native error=0，LIVE JPG/MOV/MAN 均在 |
| 9086 | PRESS 29.089 的 phase=4、job=3；WRITER 29.527 | 被忙门控忽略；无独立 ACCEPT/MUX |
| 9087 | 快门诊断 29.580/29.636；WRITER 30.042 | 仍在 9085 mux 中，无独立 ACCEPT/MUX |
| 9088 | 快门诊断 30.089/30.136；WRITER 30.547 | 仍在 9085 mux 中，无独立 ACCEPT/MUX |
| 9089 | PRESS 37.377 → FREEZE 38.883（90 帧）→ MUX 38.900–43.516 | rc=0，native error=0，LIVE JPG/MOV/MAN 均在 |
| 9090 | JPG mtime 15:00:14.730 | 超过当前日志约 14:59:37.583 的快照，只有文件清单证据 |

9085 的 mux 历时 6.309 秒，结束后到 READY 又约 1.903 秒；9089 的 mux 历时 4.616 秒，随后约 1.986 秒重新 READY。`runtime.c:49` 明确在 `stream.job != 0` 时忽略新 PRESS；`live_stream.c:239` 在 job 2/3 时不继续编码。两张后续连拍只有快门声诊断和原厂 writer 事件，不能伪造为日志中有三条独立 rejected PRESS。

## 音频、取消与双卡

9085 的 AUDIO=0x303，选择 v55 起的静音后备音轨；9089 的 AUDIO=0x202，选择实际冻结音频。两者事务都成功。9085 的 freeze 后约 1.492 秒才进入 mux，与等待音频窗口后跳过的路径一致，但该时间关系不能单独确定底层音频失败原因。

没有 FREEZE_FAIL、ENC_ERR 或 MUX_ABORT；两次 MUX_END 均 rc=0/native error=0。快门期间的 SOFT_CANCEL 由既有 soft-cancel 路径确认后保留正在进行的工作，不能将其名称直接解释成 Live 事务取消。进入回放引起的 RELEASE 在第二次成功保存以后。

新 MAN 的 native 路径均为 B 盘。`volume_generation=3`（9085/9089）和 `2`（9090）是 runtime 的 `card_epoch` 标记，数值变化本身不能证明配对损坏。日志没有 `MUX_END a&0x100` 的 twin 事务事件；现有清单只来自一个挂载卷，不能证明另一张卡有无文件或实际双卡设置。

## 边界

日志序列没有保存 PB 的独立 `vring`（`log_step` 只写 crumbs 和 stream ring），因此不能由没有 PB 事件断言回放模块没工作。9085/9089 的完整成功文件仍无标志，应继续检查回放 lookup/metadata 识别。9090 的详细拍摄过程不在这两份日志中。

来源为 `card-20261115/inventory.json` 和本地复制的两份日志，解析值、完整事件、来源 SHA256 及生产逻辑位置见 `capture-log-analysis.json`。没有访问相机、USB 或原卡，没有修改生产源码。
